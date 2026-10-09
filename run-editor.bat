@echo off
REM Mission Editor launcher for MC2 OpenGL Remastered v0.4 beta.
REM
REM Resolves paths relative to this .bat file so the install works regardless
REM of where the user extracted the zips.
REM
REM MC2_EDITOR_TRACE=1: writes editor-startup.log to CWD for launch debugging and
REM logs per-unit brain .abl open attempts + imported patrol waypoints to
REM editor-stderr.log. Keep this on so logs are always captured for modder bug reports.

REM background-launch: allow interactive user launcher. The MFC editor frame maximizes and takes focus on start and has no offscreen mode, so under CLAUDECODE (an agent shell) this refuses to run; agents use scripts/run_editor_smoke.py (nofocus.spawn).
if defined CLAUDECODE (
    echo run-editor.bat: refusing to launch the editor from an agent shell - it would take focus. Use scripts/run_editor_smoke.py.
    exit /b 3
)

set MC2_EDITOR_TRACE=1

cd /d "%~dp0"
"%~dp0Mission Editor.exe" 2>"%~dp0editor-stderr.log"
