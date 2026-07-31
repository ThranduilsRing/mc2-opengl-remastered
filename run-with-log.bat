@echo off
REM Captures mc2.exe stderr to stderr.log next to the exe.
REM Use this if mc2.exe crashes or shows a black screen — attach stderr.log to bug reports.
REM
REM THIS IS AN INTERACTIVE LAUNCH. The game window is visible and the engine
REM grabs the cursor, exactly like starting the game normally — that is correct
REM when you are sitting in front of it. It is NOT an unattended harness, even
REM though automation has repeatedly used it as one.
REM
REM CURSOR-GRAB-FIX-1: for an UNATTENDED run set MC2_WINDOW_OFFSCREEN=1 before
REM calling this. The engine then parks its window off the desktop AND refuses
REM every cursor grab, clip and warp — offscreen now implies hands-off-cursor,
REM which it did not before. The line below also pins MC2_NO_CURSOR_GRAB so the
REM guarantee does not depend on the exe being new enough to know that.

cd /d "%~dp0"
set MC2_LOG=1
if defined MC2_WINDOW_OFFSCREEN set MC2_NO_CURSOR_GRAB=1
mc2.exe 2> "%~dp0stderr.log"
