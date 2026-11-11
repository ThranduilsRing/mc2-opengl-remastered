# Water/cliff slice A

Assignment: slot07, claude/water-cliff-a, base d77c0c803.
Proof: E:/mc2-archive-2026-10/proofs/water-cliff-lowangle-1/.
Install: E:/mc2-installs/water-cliff-a (already seeded by interrupted worker).

Report consumed: glint/cliff/ridge/ring pre-W147; facets/coverage/beading
UNPROVEN. No history reruns. Current ops 24h plan supersedes gpu-avg-1 and
reserves water files for this lane; canonical bookmarks passed explicitly,
no shared bookmark edits (current ops file-conflict map).

Plan: owner payload/settings/PNG-size all13 repro; isolate geometry, normal
filter, vertex light, reflection and sun; fix established cause; offline
shader contracts/tests; local commit, lint, final build/deploy/smoke;
day all13 and night A/B; personally inspect montages and crop metrics.
At start tree clean. No engine changes or acceptance claims yet.

Remaining: slice B shoreline/coverage/ring; slice C cliff/ridge, in order.
HZB camera save failure: capture-tooling follow-up.

## Blocked handoff, 2026-11-11

No engine edit; no symptom fixed. Do not treat this as completed slice A.
Existing water tests: 12 passed (wave/glint + near gloss, 0.19s).
The owner image is **3717x1608**, not its viewer-scaled 2048x886 preview.
All original image dimensions are archived in owner-image-sizes.json.
Options match v0.4 SHA256 bc6e6388edb1d1599e1a18b86a3058cce12337c5d0e32917d250c6eb2e9f8dd1.

Concrete blocker: the previously seeded lane install contains read-only
data/textures/detail_displacement.tga. Windows denied copy2, then the canonical
payload publisher denied os.replace of its payload-tmp onto that file.
The target and source file are byte-identical (SHA256
f754ec7f656acc259d1f2f961f3125d9f2869ad9ee8dd87302f21eff5c6cc847).
No protection was cleared; no alternate write method was attempted after that
publisher permission failure. The earlier publisher invocation against the
live install was also invalid: --from-payload needs files/<relative path>,
not the install layout. Correct staging verified every manifest row first.

The seeded content additionally contains eight fern files absent from v0.4
(four data/vegetation/cards/fern_0[1-4].png and four
data/vegetation/cooked/fern_0[1-4].ktx2). No missing/size-differing reference
files were found: 29,932 expected files / 7,385,412,068 bytes. This is NOT parity
PASS because of those extras. A publisher temporary file also remained.
The parity audit happened during the failed publisher run: its executable hash
is intermediate, not a valid BEFORE identity. No capture used this install.
Do not run the partially published lane install as an accepted payload.

Named follow-up: **install-asset-parity-1 / orchestrator** must supply a writable,
v0.4-parity lane install using the approved isolation/deploy workflow. This
lane does not override lane-rules section 3's permission-refusal rule.
Then resume **water-cliff-a** at the repro gate with normal HUD, owner options,
3717x1608, MC2_HARNESS_GPU=xtx and all13 canonical bookmarks explicitly passed.
Check 4s fixed phase versus an identified later phase if the first series still
misses facets; archive both controls. No historical reruns are needed.

NOT RUN: valid BEFORE capture, facets isolation, glint fix, shader-contract
rows, shader compile, lint/build, successful deploy, smoke, final day/night A/B,
montage acceptance, glint crop metrics. Those depend on a valid replay arm.
Root cause for every engine symptom remains UNKNOWN. B/C are still unfixed;
their named serial follow-ups are water-cliff slice B and slice C.
