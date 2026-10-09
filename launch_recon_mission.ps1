# Launch mission with GOM_RECON instrumentation enabled
# Usage: .\launch_recon_mission.ps1 -Mission mc2_01 -Duration 30

param(
    [string]$Mission = "mc2_01",
    [int]$Duration = 30
)

# background-launch: allow interactive user launcher. When CLAUDECODE is set (an agent shell) it exports the no-focus env so the engine stays in the background: windowed, parked offscreen, no cursor grab, silent audio.
if ($env:CLAUDECODE) {
    $env:MC2_NO_CURSOR_GRAB = "1"
    $env:MC2_WINDOWED = "1"
    $env:MC2_WINDOW_OFFSCREEN = "1"
    $env:SDL_AUDIODRIVER = "dummy"
}

# Set environment variable for GOM recon instrumentation
$env:MC2_GOM_RECON = "1"

Write-Host "Launching $Mission with MC2_GOM_RECON=1 for $Duration seconds..."
Write-Host "Game executable: A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe"
Write-Host "Mission: $Mission"
Write-Host ""

# Launch game with mission and duration
& "A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe" -mission $Mission -duration $Duration

Write-Host "Mission completed."
