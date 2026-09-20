@echo off
setlocal
set "IDF_PATH=C:\esp\v6.1\esp-idf"
set "IDF_TOOLS_PATH=C:\Espressif\tools"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python_env\idf6.1_py3.11_env"
set "ESP_IDF_VERSION=6.1"
set "PATH=C:\Espressif\tools\python_env\idf6.1_py3.11_env\Scripts;C:\Espressif\tools\python;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\idf-exe\1.0.3;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\esp-rom-elfs\20241011;%PATH%"
set "ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011"
cd /d "%~dp0"
"C:\Espressif\tools\python_env\idf6.1_py3.11_env\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" build
exit /b %errorlevel%
