@echo off
setlocal
set "IDF_PATH=C:\esp\v6.1\esp-idf"
set "IDF_TOOLS_PATH=C:\Espressif\tools"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python_env\idf6.1_py3.11_env"
set "ESP_IDF_VERSION=6.1"
set "PYTHONUTF8=1"
set "ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011"
set "PATH=C:\Espressif\tools\python_env\idf6.1_py3.11_env\Scripts;C:\Espressif\tools\python;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\idf-exe\1.0.3;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\esp-rom-elfs\20241011;%PATH%"
set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%build-deepseek-smoke"
set "SDKCONFIG=%BUILD_DIR%\sdkconfig"
set "SDKCONFIG_DEFAULTS=%PROJECT_DIR%sdkconfig.defaults.deepseek_smoke"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if errorlevel 1 exit /b %errorlevel%
copy /Y "%PROJECT_DIR%sdkconfig" "%SDKCONFIG%" >NUL
if errorlevel 1 exit /b %errorlevel%

rem The project sdkconfig may come from a Route-A build.  Remove transport
rem symbols owned by that profile before applying the smoke defaults; IDF
rem preserves explicit values from an existing sdkconfig over defaults.
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$p = [IO.Path]::GetFullPath('%SDKCONFIG%'); $names = 'CONFIG_MIBOT_NET_BRIDGE|CONFIG_MIBOT_SHARED_NET_BRIDGE|CONFIG_LWIP_IP_FORWARD|CONFIG_LWIP_IPV4_NAPT|CONFIG_LWIP_IPV4_NAPT_PORTMAP'; $lines = Get-Content -LiteralPath $p | Where-Object { $_ -notmatch ('^((' + $names + ')(=|$)|# (' + $names + ') is not set)') }; Set-Content -LiteralPath $p -Value $lines -Encoding ascii"
if errorlevel 1 exit /b %errorlevel%

"%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" -B "%BUILD_DIR%" -DSDKCONFIG="%SDKCONFIG%" -DIDF_TARGET=esp32s3 reconfigure
if errorlevel 1 exit /b %errorlevel%
"%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" -B "%BUILD_DIR%" build
exit /b %errorlevel%
