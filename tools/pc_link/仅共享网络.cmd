@echo off
setlocal
chcp 65001 >nul
set PYTHONUTF8=1
set PYTHONUNBUFFERED=1
cd /d "%~dp0\..\.."
where python >nul 2>nul
if errorlevel 1 (
  echo Python 3 is required. Install it and retry.
  pause
  exit /b 1
)
python tools\pc_link\launch.py --manual-voice %*
set "task_exit=%errorlevel%"
echo.
pause
exit /b %task_exit%
