@echo off
setlocal
set PYTHONUTF8=1
if not exist "%~dp0.venv\Scripts\python.exe" (
  echo Run setup.cmd first.
  pause
  exit /b 1
)
"%~dp0.venv\Scripts\python.exe" "%~dp0tools\mod_rebuild.py"
echo.
pause
