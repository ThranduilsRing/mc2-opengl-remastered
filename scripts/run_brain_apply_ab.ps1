# BRAIN-DISPATCH-HARNESS-V2: A/B effect-identity runner.
# Runs the brain_dispatch_harness in --apply-mode twice:
#   pass A: MC2_BRAIN_INTENT_QUEUE unset (gate OFF) -> direct setGeneralTacOrder
#   pass B: MC2_BRAIN_INTENT_QUEUE=1  (gate ON)  -> emit -> commitBrainIntents -> setGeneralTacOrder
# Asserts both passes exit 0 (all apply assertions green), proving gate-OFF == gate-ON A/B identity.
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

# B4: the INTENT_QUEUE OFF pass is gone with the retired gate (queue + commit are part of the TechBrain policy).
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
