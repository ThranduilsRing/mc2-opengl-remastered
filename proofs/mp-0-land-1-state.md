# mp-0-land-1 budget checkpoint
Assignment: slot09, claude/mp-0-land-1. Local commits; no mp-0e or mods.
Reuse E:/mc2-installs/mp-0d-det-math and Linux slot1 source/build/install.
Archive: E:/mc2-archive-2026-10/proofs/mp-0-land-1 (receipt names below relative).

Implemented P1/P2 at 5312f977e: OpenLibm0.8.7 notice, mechanical .cpp extraction,
IRINT fork removal, include order, real quat/acos/atan2/Rotate + atan-range tests.
610801e55 commits native pins and NaN-payload routing witnesses.
Play83e090e20 merged first (59483a50e). Orchestrator then required play8f25ac0c2
before another game: merged as 761b7c3ba3720d8713a4dfdfffa61f0ba3fd389d.
This changes engine code: prior native receipts DO NOT prove this new tip.

GREEN at5312f977e: both builds/deploys, lint, mechanical-move.log;
Linux full units4168 cases; scoped sim/FP contracts93 Python tests.
All6 Linux re-pins + two-run native checks PASS: linux-native/, linux-pins/.
All5 requested Windows re-pins + two-run checks PASS: windows-pins/ first4,
windows-resume/ for24. Cross-host chains equal on all5 requested maps:
01=42b12a67031f046d;03=39a6ef01d03af2d1;10=21e2aded727ff578;
16=da93a58a5f063a59;24=63370194499cb37d. First4 native logs compared tick-by-tick
including strict. Linux battle=f179162bd3584ad4; Windows battle interrupted for steer.
GREEN at761b7c3ba: both engine builds; Linux full units4174 cases PASS
(9 deliberate expected assertion failures, overall doctest SUCCESS); scoped sim/FP
contracts and lint PASS. Receipts: merged-{windows,linux}-build.log,
merged-linux-unit.log, merged-contracts.log, merged-lint.log.
Windows deploy PASS (--skip-kit). Linux corrected deploy result in merged-linux-deploy.log.

NEXT DELTA (not complete / not mergeable yet):
1. Re-pin BOTH hosts at merged runtime761b7c3ba (or newer required merge), native
check all maps and compare full chains/ticks. Any mismatch: report map + first tick.
Only AFTER equality delete linux/*.json and baseline_path platform-directory switch.
Root battle.json is absent. Existing pins intentionally retain platform split.
2. PC second-XTX routing: verify [VK_DEVICE] UUID contains ...2700... .
Day/night smoke-lite, mc2_01 day BEFORE(play)/AFTER capture, visual pose inspection
are NOT RUN. No claim about unchanged feel/poses established. Saved read-only
baseline play-before.exe SHA starts0727f69ca1f1 (identity JSON beside it).
3. Commit common pins/tool; identity builds/deploy/check actual final tip.
Full scheduled board and full scripts pytest suite NOT RUN (scoped checks green).

Proof/data/run details:
- Proof-only driver repin-resume.py in archive: run from repo with --missions,
--duration, --install, --out; --linux selects Linux pins. Four native runs per map.
compare_hosts.py expects root battle pin and old six run directories; adapt only
proof script paths to final receipts. Preserve original receipts.
- Cold Windows24 timed out cooking imported tree GLBs, ZERO SIMID frames; archived
pc-timeout-mc2_24/. ONE supported warm --duration60 retry passed run AND check.
Not a sim divergence. Second startup failure of same class needs owner/orchestrator
decision; do not retry again. No extra games launched after mandatory steer.
- Both datasets at5312 matched after declared-asset refresh: stock digest
1c74588878defb45 (play compass changed). data_overlay.json updates only stock_digest;
two gaea fixtures unchanged. v04-assets.sha256 lists422 declared hashes.
New deploy/merge may require fingerprint reconciliation again. Do NOT restore
older shared /workspace/v04-assets.tar (stale assets/compass). Use compressed hash
deltas and tar --unlink-first to avoid mutating shared install hardlinks.
- Box slot.sh/slot_sync.sh: A:/Games/mc2-ops/vast; ssh root@ssh7.vast.ai port26221,
key C:/SPB_Data/.ssh/mc2farm_ed25519. Linux deploy needs --exe-name mc2 with positional
/workspace/slot1/install --ninja --allow-new-target --classic-assets. Omitted-name
attempt rejected provenance BEFORE changes; corrected command preserves guards.
- Linux native proofs pulled and .pulled marked. Reuse box build/install; keep
useful box scratch. PC native source artifacts retained as proof copies. Previously
refused inherited cook-report deletions remain refused; leave them alone.
- Owned Windows battle/queued smoke process trees stopped for steer. No owner,
golden or mod installs written; no push. Release reaps local slot scratch; retain
proof archive and reusable build/install. Read final release log for cleanup result.

Budget: original150-tool cap; checkpoint near cap, continuation required for P0 and
visual closure. Largest avoidable cost: ~12min raw2.9GB asset tar transfer; use
compressed hash-delta next time. Build/deploy/test proof tip761b7c3ba; native5312f977e.
