@echo off
rem 烧录 deepseek_smoke 构建到 ESP32-S3。用法: flash_deepseek_smoke.cmd COM7
setlocal
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM7"
set "IDF_PATH=C:\esp\v6.1\esp-idf"
set "IDF_TOOLS_PATH=C:\Espressif\tools"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python_env\idf6.1_py3.11_env"
set "ESP_IDF_VERSION=6.1"
set "PYTHONUTF8=1"
set "ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011"
set "PATH=C:\Espressif\tools\python_env\idf6.1_py3.11_env\Scripts;C:\Espressif\tools\python;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\idf-exe\1.0.3;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\esp-rom-elfs\20241011;%PATH%"
set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%build-deepseek-smoke"

"%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" -B "%BUILD_DIR%" -p %PORT% flash
exit /b %errorlevel%
