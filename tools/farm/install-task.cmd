@echo off
setlocal
set "PYTHON=%~1"
if not defined PYTHON set "PYTHON=%LOCALAPPDATA%\Python\bin\python.exe"
schtasks /create /f /tn MC2FarmRunner /sc ONLOGON /ru "%USERDOMAIN%\%USERNAME%" /it /rl LIMITED /tr "\"%PYTHON%\" C:\mc2farm\runner.py --root C:\mc2farm"
if errorlevel 1 exit /b %errorlevel%
schtasks /run /tn MC2FarmRunner
