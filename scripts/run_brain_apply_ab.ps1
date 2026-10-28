# Brain apply assertions under the TechBrain policy.
# Queue emission and commit are unconditional on this path; the former A/B gate is retired.
#
# Usage (from worktree root):
#   powershell -NoProfile -File scripts/run_brain_apply_ab.ps1 [-HarnessExe <path>] [-ManifestPath <path>]
#
# Defaults:
#   HarnessExe   = build64-brain-harness\RelWithDebInfo\brain_dispatch_harness.exe
#   ManifestPath = tests/fixtures/brain_runtime/manifest.json

param(
    [string]$HarnessExe   = "build64-brain-harness\RelWithDebInfo\brain_dispatch_harness.exe",
    [string]$ManifestPath = "tests\fixtures\brain_runtime\manifest.json",
    [string]$FixtureDir   = "tests\fixtures\brain_runtime"
)

$ErrorActionPreference = "Stop"
$worktreeRoot = Split-Path -Parent $PSScriptRoot

# Resolve harness exe relative to worktree root if not absolute
if (-not [System.IO.Path]::IsPathRooted($HarnessExe)) {
    $HarnessExe = Join-Path $worktreeRoot $HarnessExe
}
if (-not (Test-Path $HarnessExe)) {
    Write-Error "Harness exe not found: $HarnessExe"
    exit 1
}

function Run-HarnessApply {
    Write-Host ""
    Write-Host "=== brain_dispatch_harness --apply-mode (TechBrain policy) ==="
    & $HarnessExe --manifest $ManifestPath --fixture-dir $FixtureDir --apply-mode
    return $LASTEXITCODE
}

# Queue + commit are part of the TechBrain policy.
$exitB = Run-HarnessApply

Write-Host ""
Write-Host "=== Summary ==="
Write-Host "exit: $exitB"

if ($exitB -eq 0) {
    Write-Host "PASS: brain apply harness green"
    exit 0
} else {
    Write-Host "FAIL: harness failed"
    exit 1
}
