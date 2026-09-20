@echo off
setlocal
set "MIBOT_TEST_PYTHON=C:\Espressif\tools\python_env\idf6.1_py3.11_env\Scripts\python.exe"
if not exist "%MIBOT_TEST_PYTHON%" (
  echo ESP-IDF Python was not found at:
  echo   %MIBOT_TEST_PYTHON%
  echo Update MIBOT_TEST_PYTHON in wifi_led_test.cmd or run tools\wifi_led_test.py with Python 3.9+.
  exit /b 1
)
"%MIBOT_TEST_PYTHON%" "%~dp0tools\wifi_led_test.py" %*
exit /b %errorlevel%

