# MW5 basis gate state

- Assigned branch/worktree verified; merged claude/play-1113 at e9dec9764.
- Plan: fresh glTF/PSK and nine PSA exports; derive basis from all reference bones;
  implement shared mapping, numerical verification and synthetic tests; offline checks.
- Inputs/derived bytes: G:/mc2-tmp/mw5-basis-gate-1 only, deleted at hand-back.
- Proof receipts: E:/mc2-archive-2026-10/proofs/mw5-basis-gate-1.
- No engine build, run, install or deployment authorized in this lane.
- Implemented shared basis, REFSKELT parser, numeric verifier, hard mesh/IBM/name
  gates and seven synthetic tests (all pass, including byte-equal GLB builds).
- Fresh umodel exports: -nogltf is unsupported; documented -psk works instead.
- All 121 reference bones pass: max rotation 0.000089610 degrees,
  max translation 0.000005404 m; inverse-bind drift 0.000033764.
- Basis: position (x,z,-y)/100; child quaternion (x,z,-y,-w), root +w.
  Reference root is identity; Stand world-pelvis anchor selects root +w:
  0.005368529 degrees against rest, versus 179.996203877 for alternate -w.
- Stand FK delta 3.773942213 m; left-side separation 5.771804374 m;
  walk range ratio 0.921480176, correlation -0.880624460, phase 0.459016393.
- Real GLB conversion correctly BLOCKED: 99/15222 mixed triangles (0.650374%),
  over the required 0.5%; no threshold relaxation or replacement art produced.
- Next: record final receipts and commit. Follow-up requires a decision on the
  mesh's cross-bone seams before this asset can pass the strict split gate.
