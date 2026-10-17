@echo off
chcp 65001 >nul
title pendulum-g PC WebUI
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Python not found in PATH.
  echo Please install Python 3.9+ from https://www.python.org/downloads/
  echo and tick "Add python.exe to PATH" during setup.
  pause
  exit /b 1
)

python -c "import bleak" >nul 2>nul
if errorlevel 1 (
  echo [SETUP] Installing dependency: bleak ...
  python -m pip install bleak
)

echo.
echo Starting PC WebUI ... browser will open automatically.
echo If the board is not powered on yet: power it on, then click
echo "连接 pendulum-g" on the page. No board? Click "服务自检".
echo.
python pc_web_server.py %*
pause