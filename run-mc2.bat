@echo off
REM Launches the game.
REM If mc2.exe doesn't start, try run-with-log.bat instead — it captures errors.

cd /d "%~dp0"
mc2.exe
