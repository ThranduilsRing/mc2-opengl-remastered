# Water/cliff slice A

Assignment: slot03, claude/water-cliff-a, base d77c0c803.
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
Initial tree was clean. Source repair and final evidence are recorded below.

## v7: fresh W148 install and lower-altitude replay

Engine source remains c4f33309c; approved glint a52c4ccc6 is preserved.
Orchestrator authorized fresh E:/mc2-installs/water-cliff-a2. Original refused
water-cliff-a install was untouched. Published the complete v6/owner-complete
payload through deploy_payload.py with W148 head1fdbb7c6c114: rc0,6107 verified
manifest rows; executable SHA25678229b19db8afebbfdddaf38bf312184550dfedf8cb31fdb14adbad7b0b7bd4c.
Supported asset parity sync rc0:26145 files, zero missing/size/content/extra.
Reference options copied to the absent target and their hashes recorded.

Initial startup crashed at frame0,0xE0004D46 invalid-parameter. The payload
manifest and asset parity omit bootstrap root configs; system.cfg,prefs.cfg,
minprefs.cfg,orgprefs.cfg were absent. Copied only those absent files from the
read-only reference, matching make-isolated-deploy.py:198-204 seed policy.
Crash/catalog/matching PDB archived under
E:/mc2-archive-2026-10/crashes/water-cliff-a2-2026-11-11-v7/.

Corrected offscreen capture rc0: all13 canonical plus20 altitude-only variants
at100/70/50/35/25 percent for100624,100638,100705,101350,101454. Fixed30Hz,
trigger600,settle30,3717x1608,normal HUD,XTX,original options. All20 reduced
altitudes honored; XY/rot/proj and rendered origins match their base poses.
Canonical101157 requests28000 but captures12515.314 due to camera clamping;
focus Z follows terrain (gos_visual_capture.cpp:668-695), both explicitly
recorded in the camera audit. Initial overstrict audit failed on terrain Z;
corrected audit preserves requested and effective values rather than claiming
exact replay of unreachable inputs. Canonical bookmarks were not modified.

Opened both owner facet PNG references, five altitude sheets and all13 sheet.
Large triangles in the owner reference remain UNREPRODUCED at every tested
altitude. The regular fine ripple lattice IS visible. Per-pose observations:

- 100525: fogged map corner, no large water triangles visible.
- 100537: lower-pitch map corner, no large water triangles visible.
- 100624: cliff reflection and fine regular ripples at all five altitudes.
- 100638: cliff/trees dominate increasingly as altitude falls; water smooth.
- 100705: regular ripples and cliff reflection, no owner-scale facets.
- 100810: overhead point/shore, water lacks large facet patches.
- 100933: road/shore foreground, water shows fine ripples.
- 101056: cross-channel cliff and reflected trees; no large facet patches.
- 101157: fog obscures scene at clamped maximum; no useful facet conclusion.
- 101247: airfield/runway pads present, edge water remains smooth.
- 101350: broad original W148 glint and regular ripple lattice at every height;
  owner screenshot's large triangular patches do not appear.
- 101454: enlarged cliff reflection as camera lowers, fine ripples persist.
- 101648: shallow cyan shore shelf and fine ripples, no large facets.

Evidence: E:/mc2-archive-2026-10/proofs/water-cliff-lowangle-1/v7/.
before-deploy.log,before-parity.log,before-capture-bootstrap.log,
before-altitude-bootstrap/,before-camera-audit.json,altitude-*.jpg,
before-canonical-all13.jpg. Existing wave/glint and near-gloss test modules
PASS before game runs (offline-water.log). No shader source change; production
compile,shader rows,lint,new build,smoke and candidate day/night A/B NOT RUN.

PARTIAL: item1 sweep complete but reproduction gate failed; facet isolation
and repair items2-3 remain blocked. Item4 stochastic change remains pending
its explicit after-facet ordering. Candidate a3 not created without a repair.
No visual acceptance or owner-install delivery claim. Source legacy random
vertex heights (water_geom.vert:177-202) remain a hypothesis, not a proven cause.
Per lane rule2, stop after the prior v4 and this v7 failed reproduction class;
do not request another replay without changed evidence. Named follow-up
water-cliff-a needs a reproducing effective water tuning/time/session or
owner-confirmed different input before source isolation, then separate facet
and stochastic commits with all13+night proof. Fresh a2 is kept for orchestrator
inspection; old installs are kept per override. No push or upload.

## v4 resume (2026-11-11 22:20 UTC)

Install repair 94b6ebb24 merged at a9d5ed1ac; normal deploy in progress.
Owner BEFORE payload will be staged from its verified manifest and published
into this lane only. No repeated historical captures. Repro controls:
3717x1608, normal HUD, XTX, original options, canonical all13 bookmarks.
Next: complete parity/identity audit; owner BEFORE all13; isolate water inputs;
fix demonstrated cause, offline proof, commit/build/deploy, smoke/day/night A/B.
Slice B/C remain named serial follow-ups. Run cap 150 calls / 120 active min.

22:35 UTC: normal deploy succeeded, owner payload verified/published; full content
parity 29,932 files / 7,385,412,068 bytes. Four cookstamps omitted by hygiene
were restored read-only from v0.4 before valid runs. Owner all13 at frame120
and gameplay frame600 show broad glint, not large facets. BaseQuality0 also
smooth; facets remain UNKNOWN. WaterReflections0 retains broad direct glint.
Shader reload diagnostic rejected ABI mismatch; not bypassed. Publisher
restored the original owner shader payload and pruned its candidate SPIR-V.
Candidate removes the duplicate broad sun lobe and reduces primary gain3 to1;
normal/roughness AA, geometry, terrain, assets untouched. Existing water tests
12 PASS; 79 production shader compiles PASS. Required layout rows in progress.
Immediate base all13/day and mc2_10/night in progress before candidate build.
Next: commit, lint, one final Windows build/deploy/smoke and final day/night A/B.
No facet acceptance claim; concrete visible glint fix must be judged in pixels.

Remaining: slice B shoreline/coverage/ring; slice C cliff/ridge, in order.
HZB camera save failure: capture-tooling follow-up.

## v4 result: visible glint fix, partial slice

Source a52c4ccc63871ac2a0989aa4a15ee691a875d301: remove duplicate broad sun
lobe and gain3 amplification. Offline water tests12 PASS, production shader
compiles79 PASS, selected shader/Slang/interface/UBO rows PASS. Windows
RelWithDebInfo build0, deploy0, smoke-lite mc2_01+mc2_10 PASS.
Required lint FAIL is stale generated docs/background-launch-inventory.md,
unchanged from base; merge owns refresh. No ABI/resource/lifetime change.
Final base/candidate all13 day +3 night completed, matched controls validated,
ab.py comparisons completed0; personally opened all13/airfield/night montages.
At owner_101350 fixed crop near-white12.2345% ->0.06695%; bright mean row
coverage617.297 ->3.196px (threshold coverage, not physical sun diameter).
Airfield terrain/pads preserved; intended water edge dimmer. Night southeast
moon highlights reduce, other night views visually unchanged.
Proof: E:/mc2-archive-2026-10/proofs/water-cliff-lowangle-1/v4-report.md,
v4-final-capture-audit.json, v4-ab-day-complete/, v4-ab-night-complete/.
UNFIXED: facets unreproduced even with owner exe/shaders/options at3717x1608
and gameplay HUD; need owner's live water tuning/session state before another
facet continuation. Slice B shoreline/coverage/ring and slice C dark cliff/ridge
remain named serial follow-ups. HZB save failure remains capture-tooling.
No production deploy or owner acceptance claim. Lane install kept for review.

## Historical blocked handoff (install blocker superseded by v4)

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
