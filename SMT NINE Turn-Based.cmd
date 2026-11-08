@echo off
rem SMT NINE Turn-Based Edition launcher
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File "%~dp0tools\turnbased\Launcher.ps1"
