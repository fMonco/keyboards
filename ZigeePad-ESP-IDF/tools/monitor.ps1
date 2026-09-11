param([string]$Port = 'COM3')
$ErrorActionPreference = 'Stop'
$monitorPython = if ($env:IDF_PYTHON_ENV_PATH) {
    Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe'
} elseif (Test-Path 'C:/Espressif/tools/python/v6.1/venv/Scripts/python.exe') {
    'C:/Espressif/tools/python/v6.1/venv/Scripts/python.exe'
} else {
    (Get-Command python -ErrorAction Stop).Source
}
& $monitorPython -u (Join-Path $PSScriptRoot 'monitor.py') $Port
