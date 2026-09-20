@echo off
setlocal
set "IDF_PATH=C:\esp\v6.1\esp-idf"
set "IDF_TOOLS_PATH=C:\Espressif\tools"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python_env\idf6.1_py3.11_env"
set "ESP_IDF_VERSION=6.1"
set "PYTHONUTF8=1"
set "ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011"
set "PATH=C:\Espressif\tools\python_env\idf6.1_py3.11_env\Scripts;C:\Espressif\tools\python;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\idf-exe\1.0.3;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\esp-rom-elfs\20241011;%PATH%"
cd /d "%~dp0"

rem Keep the normal board config as the first layer and apply Route-A last.
rem Use a separate config/build directory so a normal firmware build is not
rem silently left with IP forwarding or NAPT enabled. Do not call
rem `idf.py set-target`: that action renames the project-root sdkconfig.
set "PROJECT_DIR=%~dp0"
set "SDKCONFIG=%PROJECT_DIR%build-routea\sdkconfig"
set "SDKCONFIG_DEFAULTS=%PROJECT_DIR%sdkconfig.defaults.routea"
if not exist build-routea mkdir build-routea
copy /Y "%PROJECT_DIR%sdkconfig" "%SDKCONFIG%" >NUL

"%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" -B build-routea -DIDF_TARGET=esp32s3 reconfigure
if errorlevel 1 exit /b %errorlevel%
"%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" -B build-routea build
exit /b %errorlevel%
