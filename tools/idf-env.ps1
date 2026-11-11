# Sets up ESP-IDF v5.3.1 in the current PowerShell session. Dot-source it:
#
#   . tools\idf-env.ps1
#
# The v5.3.1 Python environment was made with Espressif's Python 3.11; without the
# two lines below, export.ps1 picks up Python 3.13 and looks for an environment that
# does not exist.

# Started from Git Bash, MSYSTEM is inherited, and idf.py then refuses to run
# (while still reporting success). It means nothing in PowerShell.
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue

$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\python_env\idf5.3_py3.11_env'
$env:PATH = 'C:\Espressif\tools\idf-python\3.11.2;' + $env:PATH

. C:\Espressif\frameworks\esp-idf-v5.3.1\export.ps1 *> $null
if ($env:IDF_PATH -and (Test-Path $env:IDF_PATH)) {
    Write-Host "ESP-IDF v5.3.1 ready. Use idf.py as usual; the build folder is shared with Espressif-IDE, so add --no-ccache to builds."
} else {
    Write-Host 'ESP-IDF v5.3.1 could not be activated'
}
