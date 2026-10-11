<!-- doc-status: current | reviewed:2026-10-11 -->
# tier2 asset_load_fail (mc2_11/12/13/16/22/24): the 0.4 install's jaegermech override has no shape

## Verdict
Not a deploy gap. `deploy_payload.py` ships the exe, shaders, generated defs and example mods; it has never carried base game data (`*.fst`,
`data/tgl/**`), and `git ls-files data/tgl` holds 7 files. The fault is in the install content: `mc2-win64-v0.4/data/tgl/jaegermech.ini`
is a loose override (4323 bytes, 2026-06-19) whose shape file does not exist anywhere in that install.

## Evidence
- Every failing log has exactly the same two lines:
  `[ASSET_LOAD v1] event=required_missing ctx=mech3d.base rc=-1 path=data/tgl/jaegermech.ase` and `event=appr_type_fail name=jaegermech`.
- The stock ini (srcdata `tgl/jaegermech.ini`, 4138 bytes) names `FileName0="Jagermech"` / `FileName1="JagermechL1"`; tgl.fst holds
  `data/tgl/jagermech.tgl` and `jagermechl1.tgl`. The loose override replaces them with a single `FileName="Jaegermech"` (and 7 weapon nodes
  instead of 5), i.e. it asks for `data/tgl/jaegermech.tgl`, which neither tgl.fst (the 0.4 install and the 10 archived installs on E: were listed with
  a small FST lister) nor the loose tree contains.
- `E:/mc2-archive-2026-10/installs/mc2-win64-abl-validate/data/tgl/jaegermech.tgl` (44528 bytes, 2026-06-23) is the missing shape; that
  install has the same 4323-byte ini next to it.
- Audit of every loose `data/tgl/*.ini` in 0.4 that also exists in tgl.fst: 47 reference an absent shape, but only jaegermech fails a load in any tier2 log (the rest
  are damage/LOD/shadow slots, which load fail-soft, or types no mission spawns), so only the six missions
  that spawn a Jaegermech fail. `tests/smoke/smoke_missions.txt` already documents this for mc2_24 (`allow_asset_load_fail=1` in tier1).

## Proof (lane 3 install, tip bcbfdb3ee)
Copied the archived `jaegermech.tgl` into `A:/Games/mc2-opengl/mc2-win64-lane3/data/tgl/` and ran `run_smoke.py --tier tier2 --mission mc2_NN`
for the six stems (fingerprint OK, no INVALID banner): mc2_11 PASS 8192 frames, mc2_12 PASS, mc2_13 PASS, mc2_16 PASS, mc2_22 PASS, mc2_24 PASS
(before the copy all six failed `asset_load_fail` with the two lines above).

## Decision for the owner
Install-side, two equivalent repairs; neither is a repo change:
1. copy `jaegermech.tgl` from the archive into `mc2-win64-v0.4/data/tgl/` (keeps the 7-weapon override), or
2. delete the loose `jaegermech.ini` (back to the stock 5-weapon Jagermech from tgl.fst).
After either, drop `allow_asset_load_fail=1` from the mc2_24 line in `tests/smoke/smoke_missions.txt` (its own comment asks for this).
A structural fix (a data-presence check in `deploy_payload.py --verify-only` that every loose `data/tgl/*.ini` `FileName*` resolves in
tgl.fst or loose) is possible but is a decision for the orchestrator: the base data is outside the payload by design.
