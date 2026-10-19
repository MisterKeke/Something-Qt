param(
    [Parameter(Mandatory = $true)]
    [int]$ProcessId,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [ValidateRange(5, 3600)]
    [int]$DurationSeconds = 60,
    [ValidateRange(250, 10000)]
    [int]$IntervalMilliseconds = 1000
)

$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputPath) {
    throw "Output file already exists; choose a new CSV path: $OutputPath"
}
$process = Get-Process -Id $ProcessId
$watch = [System.Diagnostics.Stopwatch]::StartNew()
$previousCpuSeconds = $process.TotalProcessorTime.TotalSeconds
$previousElapsedSeconds = 0.0
$samples = [System.Collections.Generic.List[object]]::new()

while ($watch.Elapsed.TotalSeconds -lt $DurationSeconds) {
    Start-Sleep -Milliseconds $IntervalMilliseconds
    $process.Refresh()
    $elapsedSeconds = $watch.Elapsed.TotalSeconds
    $cpuSeconds = $process.TotalProcessorTime.TotalSeconds
    $wallDelta = [Math]::Max(0.001, $elapsedSeconds - $previousElapsedSeconds)
    $cpuPercentOneCore = 100.0 * ($cpuSeconds - $previousCpuSeconds) / $wallDelta
    $samples.Add([pscustomobject]@{
        utc = [DateTime]::UtcNow.ToString('o')
        elapsed_seconds = [Math]::Round($elapsedSeconds, 3)
        cpu_percent_one_core = [Math]::Round($cpuPercentOneCore, 2)
        working_set_bytes = $process.WorkingSet64
        private_bytes = $process.PrivateMemorySize64
    })
    $previousCpuSeconds = $cpuSeconds
    $previousElapsedSeconds = $elapsedSeconds
}

$samples | Export-Csv -LiteralPath $OutputPath -NoTypeInformation -Encoding utf8
Write-Output "Wrote $($samples.Count) process samples to $OutputPath"
