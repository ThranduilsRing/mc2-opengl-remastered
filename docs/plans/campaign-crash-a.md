# Campaign crash A

Assigned base: d77c0c803. Branch: claude/campaign-crash-a, slot04.

The results-to-logistics crash is a null killed-object lookup in
LogisticsPilot::update. The pure pilot_kill_result helper classifies a missing
lookup as no icon and no valor credit. Ground mechs, air movers and vehicles
keep their prior icon/medal behavior. Aggregate infantry/mech/vehicle counters
remain in the original accounting block, before the lookup loop. No save fields
or simulation decisions change.

Limitation: killed records contain watch IDs, not classes. An unresolved record
cannot contribute to the ground-mech count for Valor (3) or Uncommon Valor (7),
so a medal can be omitted even when aggregate combat counters include the kill.

Offline proof: test_pilot_kill_result exercises production classification,
missing/unsupported objects, ground mech/air/vehicle decisions, and medal
thresholds. An archived comparison script pins aggregate accounting to the base.
Final-tip test, lint, compile, deployment and smoke receipts are archived under
E:/mc2-archive-2026-10/proofs/campaign-crash-a/; consult the handback there for
actual PASS/FAIL/NOT RUN status. This note does not claim pending proof passed.

Both original dumps and mc2.pdb remain archived under
E:/mc2-archive-2026-10/crashes/quiet-A-2026-11-11/.
Stdlib triage verifies their CodeView GUID/age against the PDB and captures
exception registers and instruction bytes without installing a debugger.

Texture corruption remains OPEN. No texture code changes belong to A.
Next: campaign-crash-B1 builds existing MSVC MC2_ASAN on E:, uses its isolated
install and authorized PC lease, and runs the stock campaign sweep for a
sanitizer report or NO-REPRO. B2 requires B1's report before engine edits;
deleting the write-only CPU index alone cannot establish corruption closure.
Original run configuration/correlation details and hypotheses are in the
archived triage receipt. Normal-build chain closure requires the default sweep
through campaign_complete and a separate --liveness run with 40-second windows.

Record-only unrelated reports: mc2_07 AV after capture without a bundle; CVE-G
cityassault AV twice. Reproduction prerequisites remain exact executable/PDB,
content/mod versions and load order, mission/save/commands, settings/seed,
preceding process history, logs and a fresh dump.
