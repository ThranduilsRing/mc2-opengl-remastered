# Q2: GL VFX parity oracle

Status: **DIAGNOSTIC / PARITY not accepted** for each of missile/LRM trail, PPC,
explosion and smoke, and for the complete evidence set. All twelve final captures were
collected, fingerprint-checked, cropped and viewed. GPU missile/smoke repeats fail;
PPC beam/LRM trail coverage is unproven; no effect-free or suppressed-control set exists.
No particle promotion or MLR deletion is authorized by these results.
Worktree `A:/Games/mc2-s3`, branch `claude/vfx-oracle`, executable commit
`5636f22bc357414fa75ca15c8fa52209995cae21`, clean engine fingerprint. Install:
`A:/Games/mc2-opengl/mc2-win64-lane3` only. No engine files changed; no index writes or commits.

The binding MLR design is absent from this checkout. I read the complete local Git version with
`git show d4895e56d:.claude/cleanup/mlr-retirement-design.md`, plus the current
`sim-kernel-design.md` presentation/VFX rows. Q2 calls the oracle G batch 1;
the design's migration table calls the oracle **batch 0** and the gameplay/RNG boundary **batch 1**.
Batch numbers below refer to that table, not a renumbered plan.

## Capture recipe and limits

`scripts/capture_vfx_oracle.py` calls the unchanged `scripts/run_visual_capture.py` harness.
It acquires the ordinary host lease in the same process **before** the harness starts its deadline.
The direct runner otherwise starts its clock before `nofocus.spawn` waits for the lease:
one diagnostic run spent 128 seconds queued and produced no PNG. Neither lane-owned runner was edited.

Each arm uses two separate fresh engines (`r1`, `r2`), 30-second duration, fixed 30 Hz,
1920x1080, eight settle frames, `MC2_SMOKE_FREEZE_ANIM=1`, GL FBO readback,
`MC2_DETERMINISTIC_RNG=1`, `MC2_RNG_SEED=0xC0FFEE`, and the existing force-fire fixture.
MLR pins `MC2_GPU_PARTICLES=0` (equal to the shipped default false); GPU changes **only** that master to `1`.
Earlier diagnostics left the master unset, also false on this executable.
The harness scrubs ambient MC2 settings. Oracle render/tube/shape and GPU simulation gates remain unset/default OFF.
This measures the requested master toggle, not the fully harvested candidate described for future promotion.

Final recipes park the camera at the bookmark from frame zero with `MC2_CAMERA_PARK`.
Earlier diagnostic captures moved it only after freezing the sim, confounding pre-birth visibility.
Bookmark duplicates are the same pose within an engine, required by the capture iterator;
they are **not** the same-arm repeat controls.

There is a deterministic **indirect** seed path: startup `gos_srand` at `code/mechcmd2.cpp:378`
precedes `Stuff::InitializeClasses` at `:1007`; `mclib/stuff/random.cpp:54` fills its separate
250-word table from `gos_rand`. gosFX births use that table (`mclib/gosfx/effect.cpp:552`,
`:684`; `mclib/gosfx/particlecloud.cpp:570`; `mclib/gosfx/tube.cpp:1039`).
Mission reseeding (`code/mission_init.cpp:78`) does not reset the Stuff table.
The sidecar's `seed` is the smoke seed, **not** a gosFX stream state/draw-count record.
The existing gate also affects sim RNG; it was pinned equally in both arms, never changed in engine code.
No isolated presentation-only seed/reset hook exists. Adding it would require the expressly forbidden
`mclib/` and likely `Gates/` surfaces. The design's RenderWorld VFX stream (`0x56465852`),
initial/state/u64 draw count and render-consumes-zero-RNG proof remain unimplemented.

## Bookmarks and content

New fixture-authored data: `tools/vfx_oracle/bookmarks/mc2_24.json` frames the force-fire lance;
`mc2_01.json` widens the existing `tests/visual/bookmarks/mc2_01_combat.json` Werewolf pose;
`mc2_10.json` frames stock Zeus/Highlander spawn positions and their +150 X targets.
These are not user-authored saved poses. `covers` stays empty until a visible class is confirmed.
`mc2_10` coordinates were read from lane3's `mission.fst`; its loose mission copy matches the extracted bytes.
The loose FX library and effect CSV also match their FST payloads.

Resolved lane3 input SHA256:

| Input | SHA256 |
|---|---|
| `data/effects/mc2.fx` | `0fc3f12eec5245a7544429c93fc891f0f770a4131cd4a8ca95616bf4b5361b3d` |
| `data/objects/effects.csv` | `d07337e581382a60aab6aa97668b351f0e769b3cb5489dbbc1f640dfdbdf4aef` |
| `data/missions/mc2_10.fit` | `885244171387ccdf64f1414de2d2009ac67bf35002fb7fe689342f1d0f9c656b` |
| `data/missions/mc2_24.fit` | `d554a3948e9df993c19baec80868fbb03bff835f471096b467c58d121ed10341` |
| `data/missions/mc2_01.fit` | `95c715176dd664c68f1d04f386e954ef296724a4e0919bf33fa4e43dbef2d958` |
| `options.cfg` before final samples | `438d8223767e5ece50e6d02242605080049057d74d02c1d30dfe160be0b37b80` |
| `data/missions/gamesys.fit` | `b52918ce5b2a3342e5d06b9c3942b0b50b784f46f89ac9a0d4c367bfdf17dcf7` |
| `data/objects/compbas.csv` | `f95a229dc777aef4bf6f261713c1eb56ee5da5e9ba0a86f312d36656096e93de` |
| `data/objects/object2.pak` | `dda3fd6c8e03e21fb88dd409fba3afd355f2f58fd34c9801ea9ab17a1f0d2488` |

Each capture has an engine sidecar and harness manifest recording effective environment, options,
camera, frames, actual GL backend/FBO readback and binary provenance. This is stock-scene evidence,
not a stock/mod leaf inventory or a content-wide migration acceptance.

## Digest method and reviewed diagnostic

`tools/vfx_parity_oracle.py` hashes decoded RGBA8 crop pixels:
SHA256 of little-endian u32 width, little-endian u32 height, then row-major RGBA bytes.
Rectangles are half-open `[x0,y0,x1,y1]` at 1920x1080. It rejects out-of-bounds/empty crops,
different source resolutions, and duplicate resolved source paths used as repeat controls.
Its exit code is zero only when all four families receive **PARITY**. Repeatability alone
receives **DIAGNOSTIC**, even when all same-arm hashes match. Current evidence exits 1.
Every montage contains MLR1, MLR2, GPU1, GPU2 and absolute RGB difference MLR1/GPU1.

### PARITY evidence contract (Q2-fix)

Each family must have two MLR and two GPU captures, exact same-arm repeatability and
exact MLR/GPU decoded-pixel equality. Each primary capture needs a recorded visible-family
demonstration and capture provenance (fingerprint, bookmark, frame). Coverage is a reviewed
manifest attestation: the tool measures pixels but does not recognize beams, trails or smoke.
An effect-free control and an independent effect-suppressed run are required for each arm,
with matching fingerprint/bookmark/frame and full source resolution. All eight paths must
be distinct. A positive crop must differ from its effect-free control in **more than 16 pixels**,
each with maximum absolute RGB-channel difference **greater than 8** (8-bit channels).
The suppressed run must **FAIL** this same presence test in both arms. A negative run
that still passes presence rejects PARITY; a suppressed run submitted as a positive also fails.
These fixed thresholds reject empty/noise-only regions; they do not identify an effect.

The existing `images` and `rect` fields remain unchanged. Optional `evidence` has:

- `captures.mlr` and `captures.gpu`: two records each, ordered like `images`; each record
  has `fingerprint` (12-64 hex digits), `bookmark` (nonempty pose identifier), `frame`
  (nonnegative integer), and `demonstration` (nonempty reviewed visible-family observation).
- `effect_free.mlr` and `effect_free.gpu`: records with `image` and the same provenance.
- `suppressed.mlr` and `suppressed.gpu`: records with `image`, the same provenance and
  `effect_suppressed: true`, documenting the separately captured suppression run.

Every output row includes `status`, `parity_accepted` and `parity_failures`. Complete
evidence also records measured changed-pixel counts and the negative-control verdict.
The summary requires all four families; a partial set cannot exit successfully.
No coverage or control attestation has been added to today's fixture.

Diagnostic PPC scene: `E:/mc2-archive-2026-10/vfx-oracle-q2/ppc`, frame 164,
loop fixture, camera moved at capture trigger. Region `[650,230,1300,600]`.
I viewed `E:/mc2-archive-2026-10/vfx-oracle-q2/montages-ppc-diagnostic/ppc.png`:
five mechs are static; GPU muzzle glows differ between repeats, and broad translucent patches
appear around them. There is no distinct blue-white PPC beam. **Coverage unproven; repeatability fails.**

| Arm | Repeat 1 digest | Repeat 2 digest |
|---|---|---|
| GL MLR | `61214acf6097c8a14a77796a195a18061f75af38028d531796eef14f634d0ec3` | `61214acf6097c8a14a77796a195a18061f75af38028d531796eef14f634d0ec3` |
| GL GPU | `64e79eb78dcfc4bbbfa13b30e16565eb936099d15643918a1ec80741c938cc07` | `d6d1d631e373d47fae522acba02cf202f34fb8293918a144420c1818ba5acc66` |

All four sidecars have sim frame 163, scenario time 5.433, strict mover digest `d6b1ffa7e4a3ec6d`
and visual mover digest `a07eb70ad5727122`. These are existing mover digests, not a complete authoritative sim proof.
Even inside one frozen GPU run, the identical second pose changes the region digest:
GPU1 `0d125dc25d410e0f...`, GPU2 `fea981fee995d745...`; all four MLR same-pose samples match.
Thus the sampled GPU effects still change with render frames while the sampled mover state is constant.

## PARITY GAP LIST

1. **Repeatability:** all four MLR region repeat pairs match; GPU missile and smoke drift.
   GPU PPC crop repeats match without a demonstrated beam. GPU explosion equality is
   occlusion, not a parity or complete explosion-coverage proof.
2. **Coverage and controls:** PPC crop has no demonstrated beam; missile crop has no
   demonstrated LRM trail. No effect-free pixel-difference proof or independent suppressed
   negative-control set exists. Every family therefore remains **DIAGNOSTIC**.
3. **Observed geometry/opacity:** giant opaque polygons and reduced smoke silhouettes
   are observed. The cause is **unproven**; no packet/resource diff identifies the exact
   size/material failure. Do not accept an occluded crop as equality evidence.
4. **Draw-time placeholder randomness:** Draw calls placeholder Spawn at
   `mclib/gosfx/cardcloud.cpp:747`, which consumes draws at
   `mclib/particles/spawn_cardcloud.cpp:194`, `:208`, `:233`.
   The exact first divergent draw is not instrumented/proven. The next batch needs a
   presentation-owned seed/reset/state/draw-count at `mclib/stuff/random.cpp:44`, `:84`,
   wired to births at `mclib/gosfx/effect.cpp:552`, `:684`. Reseeding alone cannot fix
   render-time draws: Draw/flush must consume zero RNG and use frozen live effect/packet state.
5. **Fixture range:** the force-fire point is +150 world X (`code/warrior.cpp:2170`; loop `:2119`).
   `GameObject::distanceFrom` converts this to 30 metres (`code/gameobj.cpp:1928`,
   `mclib/terrain.cpp:21`). Lane3 gamesys `[WeaponRanges]` sets Long min=60;
   cell-diagonal buffer is about 12.07 m (`mclib/move.cpp:714`, `mclib/terrain.cpp:33`,
   `mclib/terrain.h:50`). Thus 30 < 47.93 m: `Mover::weaponInRange` rejects it
   (`code/mover_combat.cpp:658`), and `BattleMech::fireWeapon` returns at `code/mech_weapons.cpp:117`.
   Clan ER PPC is Long in the resolved compbas CSV (component ID 154, physical line 156), as are LRM racks.
   This explains why a nonzero aggregate weapon count is not PPC/LRM coverage.
   Replace this 30 m fixture with an **on-map unobstructed ~100 m (~500 world units)**
   target and verify actual PPC and LRM shots in-frame in the next capture batch.
   No range/gameplay rule was changed to manufacture a beam in this task.
6. **Hybrid routes:** tube and shape routes remain partly MLR. Tube has a separate
   default-OFF gate; ShapeCloud/DebrisCloud/PertCloud remain MLR. Master-toggle results
   cannot authorize GPU promotion or MLR deletion, even after the oracle accepts a crop set.

## Final per-family regions, digests and viewed results

Source paths are sealed in `tools/vfx_oracle/regions.json`; full results are
`E:/mc2-archive-2026-10/vfx-oracle-q2/montages/digests.json`.
`H ×2` below is the explicit pair `(H,H)` from separate fresh engines.
These are intended-family regions; all four statuses are **DIAGNOSTIC**, with
`parity_accepted=false`. They are **not** a passing four-family coverage set.

| Family | Scene / trigger / region | GL MLR vs GL GPU result | Montage viewed |
|---|---|---|---|
| PPC | mc2_24 / 154 / `[650,230,1300,600]` | Both arms repeat in this crop; no distinct PPC beam in either. Tiny actor/lighting differences remain. Coverage unproven; no parity verdict. | [ppc.png](E:/mc2-archive-2026-10/vfx-oracle-q2/montages/ppc.png) |
| Missile | mc2_10 / 156 / `[700,180,1250,590]` | MLR shows actors and a small black blob, without a clear swept missile trail. GPU adds huge opaque triangles/strips crossing actors; their arrangement changes between repeats. Coverage unproven and repeat control fails. | [missile.png](E:/mc2-archive-2026-10/vfx-oracle-q2/montages/missile.png) |
| Explosion / impact | mc2_10 / 184 / `[680,470,850,620]` | MLR shows a layered black cloud/square cluster; GPU covers it with an opaque polygon. No resolved flame/debris sequence. Crop repeat equality is occlusion, not parity or complete explosion coverage. | [explosion.png](E:/mc2-archive-2026-10/vfx-oracle-q2/montages/explosion.png) |
| Smoke / impact | mc2_10 / 184 / `[850,240,1000,400]` | MLR's dense dark layered puff is reduced to sparse specks under GPU, with unrelated giant dark strips over it. GPU specks/strips change between repeats. Shape mismatch; repeat control fails. | [smoke.png](E:/mc2-archive-2026-10/vfx-oracle-q2/montages/smoke.png) |

I opened and inspected **all four** linked final montages, plus the earlier PPC diagnostic montage.
The impact GPU log reports `Smoke` and `Fireball` Spawn probes **before** the capture trigger
(`final/impact/gpu/r1/mc2_10_capture.log:303`, `:309`, trigger `:315`). This establishes live leaves,
not their exact per-region parent class or a valid flame/debris silhouette.

| Family / arm | Digest pair |
|---|---|
| PPC / MLR | `5f2ec57541622357c30424201069f233c38e48ecc7ced2d1867aaaa316662af0` ×2 |
| PPC / GPU | `220538057c3184d74d4c61565903447f77fdcf6c4d99ffdad621cacfb482232b` ×2 |
| Missile / MLR | `9154c43a5912427bb0133f5c1901004263726c3af531d4cbc31550da9f6ffcbd` ×2 |
| Missile / GPU | (`f5321e447d7782fe9b04ffc6f28d13717863a2fcba075e3b47a0d455ed248d34`, `6bc4016755620fbb3d89fa120afec044d596a83817ba4587f0b53a6efb3ddfa0`) |
| Explosion / MLR | `278449240335719447a85dc6ea80e96b488e2137a4547f7aee022fe2c75fa963` ×2 |
| Explosion / GPU | `fb0ceaf5648f4bf50266c8b147a45d025b189bb8c3ce06a5a44a22eb89089b27` ×2 (occluded) |
| Smoke / MLR | `43054a40a48308e5249b76c64ee9938ca8b9b2d681f8fb36c67f73ae6194c80e` ×2 |
| Smoke / GPU | (`435c12c48bc36a5bb177d21eea5ab29808fd44ab0c1609422d952b58b1092ed2`, `361e6b234bebb61bf02e6d12f15f193595484a1963bf85df14c1571207a1b7ba`) |

All four primary sidecars within each scene agree on camera/options/GL FBO readback and the following mover state:

| Scene | Sim frame / scenario seconds | Strict mover digest |
|---|---|---|
| PPC | 153 / 5.100 | `7aa10ae95c424426` |
| Flight | 155 / 5.167 | `1a9e2147cdde31ef` |
| Impact | 183 / 6.100 | `159b5a034da6cb2d` |

The primary full-frame controls also fail for GPU flight and impact; cropping has not concealed that failure.
The seeded startup path and animation freeze are **insufficient** for this GPU master-toggle oracle.
What still changes is the rendered placeholder geometry/specks at constant sampled sim state.
An isolated presentation seed hook and a frozen live effect-state/packet source are still needed;
the forbidden lane-owned engine surfaces prevent completing that part here.

## Rerun commands

Use a fresh output directory so earlier evidence remains intact:

```powershell
py -3 scripts/capture_vfx_oracle.py --plan tools/vfx_oracle/capture-plan.json --out E:/mc2-archive-2026-10/vfx-oracle-q2/rerun
```

Copy `tools/vfx_oracle/regions.json` to `E:/mc2-archive-2026-10/vfx-oracle-q2/rerun/regions.json`.
In that copy replace `/final/` in the image paths with `/rerun/`, then:

```powershell
py -3 tools/vfx_parity_oracle.py E:/mc2-archive-2026-10/vfx-oracle-q2/rerun/regions.json --out E:/mc2-archive-2026-10/vfx-oracle-q2/rerun/montages
py -3 -B -m pytest tools/test_capture_vfx_oracle.py tools/test_vfx_parity_oracle.py -q -p no:tmpdir -p no:cacheprovider
```

The capture plan intentionally retains the failing stock fixture for reproduction; a successful
PPC/missile oracle needs the legal-distance fixture correction above before blessing coverage.
Each scene saves exact harness argv and `fingerprint-checks.json`; all twelve final entries are
`DEPLOY_FINGERPRINT OK: exe sha=5636f22bc357 matches worktree HEAD` with `failed=false`.

## Verification and remaining work

VS RelWithDebInfo build of `build_fingerprint mc2 mc2_launcher`: passed (existing compiler/link warnings).
`deploy_payload.py A:/Games/mc2-opengl/mc2-win64-lane3 --allow-new-target`: passed,
865 payload files verified, manifest source `5636f22bc357`. It reported absent optional terrain layers
and existing diagnostic cook byproducts; no other install was changed.
Every completed wrapper capture prints `DEPLOY_FINGERPRINT OK` for each repeat.
Focused pytest: 26 passed after review fixes. Read-only cleanup review: ACCEPT;
duplicate-source proof hole fixed and fingerprint negative cases added.
These are the original Q2 verification results; Q2-fix results are recorded below.

`scripts/check-background-launch.py`: failed in unchanged `scripts/tests/test_board_selftests.py`
(40 raw child spawns and missing direct-spawn registration); no finding names a new oracle file.
`git diff --check`: passed. No Git Bash/check-contracts, index writes or commits are possible under this task's sandbox.
`build_all_targets.py` was not used (brief identifies its Ninja probe stall); no C++ changed, so no new doctest/build-contract harness run.

Final plan command above (output `.../final`): completed, exit 1 for GPU repeat drift.
Actual region command (`py -3 tools/vfx_parity_oracle.py tools/vfx_oracle/regions.json --out E:/mc2-archive-2026-10/vfx-oracle-q2/montages`):
completed, exit 1 for GPU missile/smoke crop drift.
The four MLR region repeat pairs match; GPU PPC matches; GPU explosion is an occluded equal crop.
Therefore the required same-arm-equality proof is **not green**.
No RenderDoc pass proof, negative-control set, VK proof, mod coverage, or promotion verdict is claimed.
All scratch/capture artifacts are retained under `E:/mc2-archive-2026-10/vfx-oracle-q2/`;
the preliminary `24-mlr`, failed `24-f164-mlr`, `ppc`, `ppc-birth`, `volley`,
`montages-ppc-diagnostic`, and final `final/{ppc,flight,impact}` plus `montages` remain for audit.
Automatic Python scratch retained: `tools/__pycache__/vfx_parity_oracle.cpython-314.pyc`
and imported helper bytecode under `tools/__pycache__/`, `tools/mc2x_import/__pycache__/`,
and `scripts/__pycache__/`. Existing `build64` outputs were rebuilt; no scratch was deleted.
[inventory.json](E:/mc2-archive-2026-10/vfx-oracle-q2/inventory.json) lists all 186 retained capture,
log, manifest, command, fingerprint, digest and montage files with byte sizes and SHA256s;
the inventory file itself is retained alongside them.

## Q2-fix verification (status correction only)

No engine changes or new captures. Focused oracle pytest:

```powershell
py -3 -B -m pytest tools/test_capture_vfx_oracle.py tools/test_vfx_parity_oracle.py -q -p no:tmpdir -p no:cacheprovider
```

Result: **42 passed**, including empty-region rejection, stable-but-wrong GPU rejection,
suppressed-run failure, rejection when suppression still passes presence, provenance/coverage
requirements, complete four-family acceptance, and today's fixture remaining DIAGNOSTIC
even with fully matching pixels and repeats (including CLI exit 1).

Existing evidence rerun, preserving the original montage/digest outputs:

```powershell
py -3 -B tools/vfx_parity_oracle.py tools/vfx_oracle/regions.json --out E:/mc2-archive-2026-10/vfx-oracle-q2/q2-fix-montages
```

Result: **exit 1**, `DIAGNOSTIC: parity_accepted=False; missing_families=[]`.
PPC, missile/LRM trail, explosion and smoke each report `status=DIAGNOSTIC`,
`parity_accepted=false`; original decoded-pixel digest pairs are unchanged.
Retained new artifacts (no files deleted):
`E:/mc2-archive-2026-10/vfx-oracle-q2/q2-fix-montages/{digests.json,ppc.png,missile.png,explosion.png,smoke.png}`.
LF line endings preserved. `git diff --check` passed; the Q2 files remain untracked,
so a separate whitespace scan covers the edited files. Git Bash/check-contracts and
index writes/commits remain prohibited; build/deploy/smoke/doctest are unnecessary
for this Python/report-only correction. The orchestrator commits the existing Q2 work
and this fix; no new read-only agent review was requested for Q2-fix.
