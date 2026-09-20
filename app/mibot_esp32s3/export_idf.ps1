# Load the locally installed ESP-IDF 6.1 environment into this PowerShell session.
# Usage: . .\export_idf.ps1

$ErrorActionPreference = 'Stop'

$idfRoot = 'C:\esp\v6.1\esp-idf'
$idfPython = 'C:\Espressif\tools\python_env\idf6.1_py3.11_env\Scripts\python.exe'

if (-not (Test-Path -LiteralPath "$idfRoot\tools\idf.py")) {
    throw "ESP-IDF was not found at $idfRoot"
}
if (-not (Test-Path -LiteralPath $idfPython)) {
    throw "ESP-IDF Python was not found at $idfPython"
}

$env:IDF_PATH = $idfRoot
$env:IDF_TOOLS_PATH = 'C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python_env\idf6.1_py3.11_env'
$env:ESP_IDF_VERSION = '6.1'
$env:ESP_ROM_ELF_DIR = 'C:\Espressif\tools\esp-rom-elfs\20241011'
$env:IDF_PYTHON_CHECK_CONSTRAINTS = 'no'

$pathEntries = @(
    "$env:IDF_PYTHON_ENV_PATH\Scripts",
    'C:\Espressif\tools\python',
    'C:\Espressif\tools\cmake\4.0.3\bin',
    'C:\Espressif\tools\ninja\1.12.1',
    'C:\Espressif\tools\idf-exe\1.0.3',
    'C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin',
    'C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin',
    $env:ESP_ROM_ELF_DIR
)
$env:Path = (($pathEntries + ($env:Path -split ';')) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique) -join ';'

# ESP-IDF's PowerShell export normally exposes idf.py as a function because it
# is a Python script rather than a native executable.
function global:idf.py {
    & $idfPython "$env:IDF_PATH\tools\idf.py" @args
}

Write-Host "ESP-IDF $env:ESP_IDF_VERSION loaded. idf.py is ready."
Write-Host "Project: $PSScriptRoot"
