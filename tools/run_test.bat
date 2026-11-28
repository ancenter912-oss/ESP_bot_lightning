@echo off
chcp 65001 >nul
echo === ESP8266 Automated Tester Launcher ===
where python >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python not found in PATH. Install Python 3 from https://www.python.org/downloads/
    pause
    exit /b 1
)
python -c "import serial" >nul 2>&1
if errorlevel 1 (
    echo [INFO] Installing pyserial...
    python -m pip install pyserial
)
python "%~dp0esp_auto_tester.py" %*
echo.
pause
