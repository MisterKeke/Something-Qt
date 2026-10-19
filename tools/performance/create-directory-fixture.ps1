param(
    [string]$Destination = (Join-Path ([System.IO.Path]::GetTempPath()) 'SomethingQt-FileExplorer-Perf')
)

$ErrorActionPreference = 'Stop'
$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$target = [System.IO.Path]::GetFullPath($Destination)
if (-not $target.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Choose a destination beneath the current user temporary directory.'
}
if (Test-Path -LiteralPath $target) {
    throw "Destination already exists; choose a new disposable directory: $target"
}

[System.IO.Directory]::CreateDirectory($target) | Out-Null
for ($index = 1; $index -le 2000; $index++) {
    $name = 'entry-{0:D4}.txt' -f $index
    [System.IO.File]::WriteAllText(
        (Join-Path $target $name),
        ('fixture entry {0:D4}' -f $index),
        [System.Text.UTF8Encoding]::new($false)
    )
}
for ($folder = 1; $folder -le 20; $folder++) {
    $folderPath = Join-Path $target ('folder-{0:D2}' -f $folder)
    [System.IO.Directory]::CreateDirectory($folderPath) | Out-Null
    $nestedPath = Join-Path $folderPath 'nested'
    [System.IO.Directory]::CreateDirectory($nestedPath) | Out-Null
    for ($entry = 1; $entry -le 20; $entry++) {
        $name = 'nested-{0:D2}-{1:D2}.txt' -f $folder, $entry
        [System.IO.File]::WriteAllText(
            (Join-Path $nestedPath $name),
            ('fixture entry {0:D2}-{1:D2}' -f $folder, $entry),
            [System.Text.UTF8Encoding]::new($false)
        )
    }
}

Write-Output "Created 2,000 root files and 20 two-level directory trees at $target"
