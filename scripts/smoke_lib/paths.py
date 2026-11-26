"""Shared PC write destinations. Reference inputs and historical archives stay on E:."""

WRITE_DRIVE = "G:"  # drive-path: ok PC write destinations; callers may override their own roots
INSTALL_ROOT = WRITE_DRIVE + "/mc2-installs"
PROOF_ROOT = WRITE_DRIVE + "/mc2-proofs"
CRASH_ROOT = WRITE_DRIVE + "/mc2-crashes"
SCRATCH_ROOT = WRITE_DRIVE + "/mc2-tmp"
STRAY_ROOT = PROOF_ROOT + "/stray-diffs"
ASSET_STORE_ROOT = WRITE_DRIVE + "/mc2-asset-store"
COOK_CACHE_ROOT = WRITE_DRIVE + "/mc2-cook-cache"
SCIFI_OUTPUT_ROOT = PROOF_ROOT + "/scifi-kit"
LEAN_SEED_ROOT = INSTALL_ROOT + "/_lean-seed"
MERGE_GATE_INSTALL = INSTALL_ROOT + "/merge-gate"
