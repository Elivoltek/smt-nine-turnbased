@echo off
rem SMT NINE Turn-Based Edition: patch your Japanese ISO (English + turn-based) and build the game
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\turnbased\Install.ps1" %*
