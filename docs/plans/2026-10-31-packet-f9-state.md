Assignment: A:/Games/mc2-lane9, claude/cxx23-packet-b, base 32f03ddbe.
Starting tip 1d9c552ee; preserved flip c099e2a81, deployed in sole install
E:/mc2-installs/claude-cxx23-packet-f. No new install.
Owner authorizes first-save baseline migration after legacy loads and exact
decoded ObjectSlot-only differences. Object masks 30 -> 0; build masks 0 -> 0.
F8 SIMID, continuation, older-reader, audit/validation and offline receipts
remain valid for identical patch. F9 evidence lives beside F8 in
E:/mc2-archive-2026-10/proofs/lane9/cxx23-f/f9-*.
Next: verify pinned native first saves, gather candidate first saves and
W87/W90 60-tick loads; decode/encode object comparison; migrate baselines in
separate commit, rebuild clean mc2+viewer once, deploy, sequential first-save
01/03/10/24 and CVE-G quickload. Keep flip only on complete green verdict.

Flip restored as 8d8149563 (exact c099e2a81 code, unchanged F8 receipts).
W87/W90 01 and 03 pass 601 contiguous post-load ticks each. W87 csvfix 03
has an empty stamp and is unsuitable; final-continuation-gate 03 matches
the checked-in W87 fixture exactly. Final archive is used for 03/10/24.
First 03 capture used speed 1 erroneously; excluded and recaptured with
the pinned speed 2. No baseline updated from the mismatched capture.
Production-codec proof converter compiled; migration checks decode/reencode
and exact native-to-tagged bytes for every object, all unrelated packets exact.

Migration prerequisites PASS (f9-gather-result.json and f9-migration-result.json).
All eight W87/W90 loads continue 601 ticks. Verified SHA256 of each historical
C/D/E source and recomposed D+E masked digest before comparing candidate saves.
Every native object encodes to the candidate bytes with the production codec,
then decodes/reencodes exactly. All other packets, FIT text, existing sections,
frame/speed/data pins and ceilings remain exact. Only required OBJS v1 is added
(flags=1, empty payload, 20 container bytes per mission). Per-class record and
byte counts are in each baseline's schema_migration.object_records.

| Mission | Objects | Native bytes | Tagged bytes | Added OBJS bytes |
|---|---:|---:|---:|---:|
| mc2_01 | 1565 | 1909496 | 1521245 | 20 |
| mc2_03 | 3134 | 3078336 | 2274527 | 20 |
| mc2_10 | 3213 | 3161904 | 2336293 | 20 |
| mc2_24 | 3283 | 3302332 | 2453053 | 20 |

Owner-authorized first-save regeneration uses clean c099e2a81 saves with the
new policy only after the above equivalence proof. The four references retain
the complete prior migration provenance. Object masks 30 -> 0, build 0 -> 0;
no other baseline/mask widened. diff --check PASS.
Next: clean committed mc2+viewer build once (-j6), explicit lane deploy, then
f9-accept.py sequential four first-save checks and CVE-G quickload; revert flip
and reference migration if any final acceptance is red.
