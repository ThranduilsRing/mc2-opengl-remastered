# Lane install fast path

Owner symptom: "Lane installs are the biggest avoidable cost tonight";
campaign-globe was "BLOCKED by a 15-min isolated full deploy".

Assignment: slot12, claude/lane-install-fast, base b26fc9bfe90a.
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
payload, verified pinned kit sources and content-addressed native recipes link
on E:; mutable runtime files and cooked kit receipts
are private copies. Failed links fall back to a copy only after checking its
additional disk cost, then verify both hashes. The private `.lane-seed.json`
receipt permits deploy to retain identical payload and atomically privatize
changed shared files before the exact-payload isolation guard. Kit-changing lanes omit
`--skip-kit`; that deliberately retains normal pinned-kit verification/cooking.
Legacy installs under E:/mc2-installs without a seed receipt also detach the
exact deploy write set when `--allow-new-target` is explicit. This includes
mission beauty assets and the deployment manifest; the isolation guard still
runs after detaching. Published installs retain their existing behavior.

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

Native recipes publish by atomic replacement, as documented by the isolation
guard. The measured seed has 664 recipes / 1,241,263,585 bytes; linking them
avoids copying these unchanged bytes for every lane. Cook JSON receipts remain
private, and an atomic recipe replacement leaves the seed inode untouched.

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
Continuation: rebased onto nifty b26fc9bfe. New evidence is the completed seed
cache plus the bake-recook-smooth refusal naming 58 shared mission assets.
The prior full install took 202.737 s; recipe linking at a5e946837 was unmeasured.
Added regression tests for legacy mission writes, shared manifest detachment,
replacement failure, warm kit hash reuse and same-size kit corruption.
Continuation prerequisite: explicit authorization assigns the unused
E:/mc2-installs/lane-install-fast-final for fresh timing and smoke-lite.
The legacy link-write fix already landed at a053001a7; its regression tests
include mission assets, manifest writes and replacement failure isolation.
Next: run gate_inputs targeted pytest plus ratchet/lint at the final tip;
time that fresh install and smoke-lite using the seed executable as the explicit
fingerprint reference (this tooling lane changes no C++). No build/deploy needed.
Record exact-tip results in the proof directory's STATE.md, then remove only the
new install after archiving runtime/crash evidence. Preserve the old
E:/mc2-installs/lane-install-fast because its prior cleanup was refused.
