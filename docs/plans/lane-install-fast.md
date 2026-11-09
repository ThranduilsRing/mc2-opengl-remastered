# Lane install fast path

Owner symptom: "Lane installs are the biggest avoidable cost tonight";
campaign-globe was "BLOCKED by a 15-min isolated full deploy".

Assignment: slot12, claude/lane-install-fast, base d63c403c9715.
No engine changes; no change to the v0.4 deploy payload.

## Use (proposed installs paragraph for lane-rules section 5)

After `lane_slot.py acquire`, use the assigned install (last branch component):

```powershell
py -3 scripts/make-isolated-deploy.py --from E:/mc2-installs/_lean-seed --to E:/mc2-installs/<branch>
```

Commit, build both targets if deploying, then deploy only the lane delta:

```powershell
py -3 scripts/deploy_payload.py E:/mc2-installs/<branch> --allow-new-target --ninja --skip-kit
```

The script refuses cross-volume sources before writing. Stock, static deployed
payload and verified pinned kit sources link on E:; mutable runtime files and cooked kit receipts
are private copies. Failed links fall back to a copy only after checking its
additional disk cost, then verify both hashes. The private `.lane-seed.json`
receipt permits deploy to retain identical payload and atomically privatize
changed shared files before the exact-payload isolation guard. Installs without
this receipt retain the original guard behavior. Kit-changing lanes omit
`--skip-kit`; that deliberately retains normal pinned-kit verification/cooking.

Seed verification is cold once, then persisted in `.lane-seed-verification.json`
and `.lane-kit-verification.json`. Reuse requires file identity, size, mtime,
and NTFS ChangeTime (not Windows creation time) plus the expected manifest hash.
Same-size corruption with restored mtime must fail. Deployment hashes the lane's
source files, retains identical verified seed bytes, and copies changed payload
to private inodes. Copied manifested bytes
are checked against expected hashes; unchanged linked files use the verified
seed inode. `.lane-seed-inventory.json` avoids repeating the stock walk and is
invalidated by changes to the seed deployment manifest. Excluded cache/mod/debug
trees are pruned before descent on a cold inventory. `--dry-run` writes no receipt.

The merge orchestrator refreshes `_lean-seed` from the verified nifty published
install after kit changes, using the existing atomic owner-seed path (run from
mc2-main after publishing nifty, never from a lane):

```powershell
py -3 scripts/deploy_payload.py E:/mc2-installs/_lean-seed --seed-owner-test --seed-source A:/Games/mc2-opengl/mc2-win64-v0.4 --source-root A:/Games/mc2-main
```

This verifies the published install against nifty and carries pinned cooked
recipes without recooking. It replaces changed seed inodes, so existing lanes
keep their old bytes. The next acquisition verifies changed files and rebuilds
the stock inventory if the deployment manifest changed. A seed kit differing
from the acquiring worktree's pinned manifest is rejected. Refresh between
acquisitions, never concurrently. For manual unmanifested stock changes, delete
the seed's inventory receipt before acquiring another lane.

## State / receipts

Proof directory: E:/mc2-archive-2026-10/proofs/lane-install-fast/.
Before: before.log, before-time.txt; stopped at 156.740 s in planning, no
target created. This is a lower bound, not a completed baseline measurement.
The source manifest lists 3,879 non-PDB files / 5,155,486,539 bytes.
Hypothesis: repeated verification of that payload dominates seed planning.
Development tests exercise corruption (including restored mtime), fallback,
pruning, inventory invalidation, kit preservation and exact isolation guards.
Cold seed verification is running; it is a one-time prerequisite for measuring
the warm common case. Next: final-tip owning gate_inputs row, ratchet/lint,
measure creation with the verified cache, archive receipts, clean owned install.
No C++ compile or deploy is needed for this tooling change; section 7's tooling
row overrides the brief's smoke request. No game run is currently planned.
