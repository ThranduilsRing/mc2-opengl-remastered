Assignment: A:/Games/mc2-lane9, claude/cxx23-packet-b, base 32f03ddbe.
F9 verdict: ACCEPTANCE RED; flip and baseline migration reverted as instructed.

History: restored flip 8d8149563; separate baseline migration 140bce296;
baseline rollback d4485a892; flip rollback d5ab47c74. Engine, masks and all
baseline bytes are restored exactly to entry tip 1d9c552ee. No limits widened.
Object run masks: entry 30 -> candidate 0 -> final 30 bytes; build 0 throughout.

Proofs: E:/mc2-archive-2026-10/proofs/lane9/cxx23-f/f9-*.
- f9-gather-result.json PASS: W87/W90 01/03/10/24, eight loads, each 601
  contiguous post-load ticks with clean c099e2a81 (exact preserved flip).
- f9-migration-result.json / f9-migration.log PASS: verified historical C/D/E
  source SHA256 and reconstructed D+E baseline digests. Every native object
  encoded with production codecs equals the candidate first-save bytes;
  decode/reencode exact. All unrelated packets/FIT/existing sections exact.
  Only required OBJS v1 added: empty payload, flags=1, +20 bytes per mission.
- Native -> tagged object bytes: 01 1909496 -> 1521245 (1565 records),
  03 3078336 -> 2274527 (3134), 10 3161904 -> 2336293 (3213),
  24 3302332 -> 2453053 (3283). Per-class totals remain in migration proof.
- f9-build.log / f9-deploy.log PASS: clean committed 140bce296, mc2 + viewer,
  preserved RelWithDebInfo Ninja tree, -j6, Idle/tool cores, explicit sole lane
  install with --require-clean. F8 offline/SIMID/audit/refusal receipts reused
  for identical code; no unchanged suite rerun.
- f9-accept-result.json: first-save checks 01/03/10/24 all PASS. CVE-G FAIL.

Exact final failure: metropolitan quicksave at tick 100, before quickload:
  STOPObject 125 covers too many cells in Save/Load!!
  [SAVE v1] reject reason=terrain_cells
  STOP [FATAL 0] Invalid building ObjectSlot
Also three existing writer diagnostics: "Mech Has too many weapon nodes to
save 12". Harness crash_silent, exit 1; no post-load ticks. Accepted ammo
errors remain 7/7; no new content error class. f9-cve-engine.log lines 1452-54,
f9-cve-battery.json and captured partial f9-cve-testgame.ims preserve evidence.

Next: investigate CVE-G object 125's footprint/count against the native
cellsCovered capacity and existing CopyTo overflow diagnostic, plus the three
weapon-node diagnostics. Do not keep a flip that cannot save this mod. After
a bounded codec/native contract fix, reapply the preserved flip and migration
commits, rebuild clean shipped executables and rerun affected acceptance.

Setup corrections: W87 csvfix 03 had an empty stamp; final-continuation-gate
03 matches the checked-in W87 fixture. Initial new 03 capture used speed 1
instead of pinned speed 2; excluded. Correct captures and green load receipts
were reused. Biggest avoidable cost: validate stamps AND pinned speed before
queuing captures. Pre-rollback migration checkpoint is f9-migration-checkpoint.md.

Only install retained: E:/mc2-installs/claude-cxx23-packet-f, still containing
rejected candidate 140bce296; Ninja output is also candidate, not rollback HEAD.
Temporary f9-cve stage/cache removed: 11446895747 logical bytes (10.66 GiB)
on E:, physical freed bytes not measured. Owner quicksave restored. No farm
job launched. Final source-tree comparison to entry tip is empty except this
state note; no further engine rebuild/test needed for the restored tree.
