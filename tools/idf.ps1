# Runs idf.py with ESP-IDF v5.3.1, whatever version the VS Code extension has selected.
#
#   tools\idf.ps1 build
#   tools\idf.ps1 -p COM5 flash monitor
#   tools\idf.ps1 erase-coredump [-p COM5]     clears the coredump partition
#
# Builds into build\ without ccache, the same as Espressif-IDE, so both can be used
# without forcing a full rebuild each time.

. (Join-Path $PSScriptRoot 'idf-env.ps1') | Out-Null
if (-not $env:IDF_PATH -or -not (Test-Path $env:IDF_PATH)) {
    exit 1
}

# Use the environment's own Python, not whichever one comes first in PATH
$python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'

Set-Location (Split-Path $PSScriptRoot -Parent)
if ($args.Count -gt 0 -and $args[0] -eq 'erase-coredump') {
    $rest = @($args | Select-Object -Skip 1)
    & $python "$env:IDF_PATH\components\partition_table\parttool.py" @rest erase_partition --partition-name coredump
    exit $LASTEXITCODE
}
& $python "$env:IDF_PATH\tools\idf.py" --no-ccache @args
exit $LASTEXITCODE
