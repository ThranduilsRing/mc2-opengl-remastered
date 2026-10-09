@echo off
REM Launches the game.
REM If mc2.exe doesn't start, try run-with-log.bat instead — it captures errors.

REM background-launch: allow interactive user launcher. When CLAUDECODE is set (an agent shell) it exports the no-focus env so the engine stays in the background: windowed, parked offscreen, no cursor grab, silent audio.
if defined CLAUDECODE (
    set MC2_NO_CURSOR_GRAB=1
    set MC2_WINDOWED=1
    set MC2_WINDOW_OFFSCREEN=1
    set SDL_AUDIODRIVER=dummy
)

cd /d "%~dp0"
mc2.exe
