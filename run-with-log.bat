@echo off
REM Captures mc2.exe stderr to stderr.log next to the exe.
REM Use this if mc2.exe crashes or shows a black screen — attach stderr.log to bug reports.

cd /d "%~dp0"
mc2.exe 2> "%~dp0stderr.log"
