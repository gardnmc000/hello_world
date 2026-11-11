# Runs idf.py with ESP-IDF v5.3.1, whatever version the VS Code extension has selected.
#
#   tools\idf.ps1 build
#   tools\idf.ps1 -p COM5 flash monitor
#   tools\idf.ps1 erase-coredump [-p COM5]     clears the coredump partition
#
# Builds into build\ without ccache, the same as Espressif-IDE, so both can be used
# without forcing a full rebuild each time.

# The v5.3.1 Python environment was made with Espressif's Python 3.11; without this,
# export.ps1 picks up Python 3.13 and looks for an environment that does not exist.
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\python_env\idf5.3_py3.11_env'
$env:PATH = 'C:\Espressif\tools\idf-python\3.11.2;' + $env:PATH

. C:\Espressif\frameworks\esp-idf-v5.3.1\export.ps1 *> $null
if (-not $env:IDF_PATH -or -not (Test-Path $env:IDF_PATH)) {
    Write-Host 'ESP-IDF v5.3.1 could not be activated'
    exit 1
}

Set-Location (Split-Path $PSScriptRoot -Parent)
if ($args.Count -gt 0 -and $args[0] -eq 'erase-coredump') {
    $rest = @($args | Select-Object -Skip 1)
    python "$env:IDF_PATH\components\partition_table\parttool.py" @rest erase_partition --partition-name coredump
    exit $LASTEXITCODE
}
python "$env:IDF_PATH\tools\idf.py" --no-ccache @args
exit $LASTEXITCODE
