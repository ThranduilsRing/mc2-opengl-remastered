@echo off
setlocal
cd /d "%~dp0"
if not "%~1"=="" goto import
set /p "campaign=Paste the campaign ZIP or folder path: "
py -3 "%~dp0tools\import_campaign.py" "%campaign:"=%" --deploy "%~dp0."
goto done
:import
py -3 "%~dp0tools\import_campaign.py" %* --deploy "%~dp0."
:done
set "result=%errorlevel%"
if errorlevel 1 echo Import failed. Read the error above. Python 3.12 or newer is required.
pause
exit /b %result%
