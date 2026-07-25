# VEHICLE-NIGHT-LIT-1 — STATUS

**Verdict: NOT FIXED. Lead hypothesis DISPROVEN by exaggeration probe.**
A real, separate latent bug was found and shipped default-OFF along the way,
but it is **not** the vehicle bug. Attribution for the reported defect is still
open. Read the "What is disproven" section before picking this up — three
theories are already dead, do not re-run them.

- Worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`
- Commit: **`b01a7a96`** — `fix(lighting): VEHICLE-NIGHT-LIT-1 — MC2-frame calc_light feed for the legacy lit program (gated)`
- Gate: **`MC2_LEGACY_LIT_MC2_FRAME`**, default **OFF** (byte-identical)
- Deploy lane used: **`mc2-win64-v0.3`** (canonical, free; `0.5-testing` is not a
  canonical deploy basename and 0.4/0.4c/0.4d-rc1 were all held by live lanes)
- Install verified against the worktree by md5 (exe + shader), see Evidence.

## The report

mc2_10 night, prison/guard-tower compound. Three trucks render as near-black
dark-brown silhouettes. In the SAME frame and SAME light pool: the mech is
clearly lit blue-grey; buildings, chain-link fences, guard towers, barracks huts
and terrain are all correctly illuminated. One truck sits inside a guard-tower
spotlight's visible ground cone and is still dark. Class-wide for vehicles, not
positional.

## Which program draws vehicles (recon, unambiguous from source)

Vehicles go through **neither** GPU batcher:

- `mclib/gvactor.cpp` contains **zero** references to `GpuMechBatcher`,
  `submitActor`, or `g_useGpuObjects` — unlike `mech3d.cpp` (mech batcher) and
  `bdactor.cpp` / `genactor.cpp` (static-prop batcher, `g_useGpuObjects`).
- Path is: `GVAppearance::render` (gvactor.cpp:2193) -> `gvShape->Render(true)`
  -> `TG_MultiShape`/`TG_Shape::Render` (tgl.cpp:3453-3478) ->
  `mcTextureManager->addRenderShape(texNode, &rs, flags)` ->
  `ShapeRenderer::render` (txmmgr.cpp:2161) -> material
  **`gos_tex_vertex_lighted`** (txmmgr.cpp:2178).
- Light feed is present and per-actor: `MultiTransformShape` calls
  `GatherLightsParameters(&lightData_)` (tgl.cpp:2840) and `TG_Shape::Render`
  calls `addLightDataStructure(&lightData_)` (tgl.cpp:3469), passed to the
  shader as `light_offset_.x`.

So on paper vehicles DO get a light slot. Root-cause class (a) — "vehicles never
get a lightDataIndex" — is **ruled out**.

## The bug that was actually found (real, but NOT the reported one)

`shaders/gos_tex_vertex_lighted.vert` fed `calc_light` **Stuff-frame** position
and normal, while the `ObjectLights` rows written by `GatherLightsParameters`
carry **MC2-frame** `light_to_world` translations and `light_dir`. The two other
`calc_light` consumers both feed MC2-frame inputs:

| consumer | position passed | normal passed |
|---|---|---|
| `mech.vert` (mechs) | `worldMC2` | MC2 (`-n.x, n.z, n.y`) |
| `static_prop.vert` (buildings/trees) | `worldPos` — MC2-frame | MC2 via `STATIC-PROP-NORMAL-FRAME-FIX` |
| `gos_tex_vertex_lighted.vert` | `WorldPos` — **Stuff-frame** | `Normal` — **Stuff-frame** |

Consequence: `length(vertex_world_pos - light_pos)` mixes frames, so `GetFalloff`
rejects every POINT / SPOT / INFINITEWITHFALLOFF light, and the INFINITE sun
`NdotL` is ~90° off. `static_prop.vert`'s in-file comment
(`STATIC-PROP-NORMAL-FRAME-FIX`, static_prop.vert:465-473) documents the
identical bug being fixed on the sibling shader — this was the last unfixed
consumer.

This is genuine and worth keeping. It is shipped **default-OFF** behind
`MC2_LEGACY_LIT_MC2_FRAME` because the program is shared by other legacy lit
shapes, so ON changes their lighting (day included).

## What is disproven — DO NOT RE-RUN THESE

1. **"The frame mismatch is why the trucks are dark."** DISPROVEN.
   Deterministic A/B on the same deployed exe (env flip only):
   | pose | changed px | verdict |
   |---|---|---|
   | `mc2_10_saved_204753` | 20,770 (4.33%) | trucks **unchanged**; delta is foliage/small props |
   | `mc2_10_saved_212019` | 6,163 (1.28%) | max per-px delta = **5** — visually nil |
   The reported trucks are the same dark in both arms.

2. **"`gos_tex_vertex_lighted` is the program that paints the trucks."**
   DISPROVEN by **verify-by-exaggeration**: the installed shader's gated branch
   was patched to emit `VertexLight = vec3(8,0,8)` (blown-out magenta) and
   re-captured at 204753. **Almost nothing in the compound turned magenta** —
   only one small vehicle-shaped object at the far top-left of the compound and
   a couple of specks. The three reported trucks did **not** change.
   => this program owns almost no pixels in this scene. Whatever draws the
   trucks is a different program.
   Evidence: `scratch/vehnightlit/zoom_204753_EXAG.png`.

3. **Root-cause class (a) "no light slot assigned"** — ruled out by the recon
   above (`GatherLightsParameters` + `addLightDataStructure` both run on the
   vehicle path).

Note the unexplained residual: the OFF/ON delta at 204753 recoloured
**foliage/small props**, yet those same pixels did **not** go magenta in the
exaggeration arm. Those two observations are not yet reconciled and are a live
thread for the next pass.

## Where the next pass should start

The remaining candidate is that the trucks are drawn by the **static-prop
batcher** (`static_prop.vert`) or the **legacy CPU triangle queue**
(`addTriangle` -> `DrawIndexedTris`, tgl.cpp:2789-2807), not by the
`addRenderShape`/`ShapeRenderer` path. Concretely:

- `TG_Shape::MultiTransformShape` has **two** emit paths — the CPU triangle
  queue (tgl.cpp:2787-2809) and the modern `addRenderShape` block
  (tgl.cpp:2819-2851, gated on `bShadersDrawPathEnabled &&
  !eligibleForGpuObjects(this) && ...`). Determine which one the truck leaves
  take. `eligibleForGpuObjects(this)` is the switch worth reading first.
- **Note the CPU vertex-lighting loop is DEAD on the modern renderer**:
  tgl.cpp:2154 `if (useVertexLighting && (Environment.Renderer != 3))`. Any
  vehicle leaf that ends up on a path expecting the CPU bake gets a stale/black
  per-vertex seed — that is root-cause class (c) and matches the
  `FAR-BACKDROP-TEXTURED-LIGHT-1` mechanism.
- **RenderDoc is now the right tool** (`tools/renderdoc/`, capture-at-bookmark
  204753, `target_resid` + `disasm` modes). Pin the draw that paints a truck
  before writing any more code. The exaggeration probe is cheap and decisive —
  use it again on whichever program RenderDoc names.
- Latent bug spotted in passing, unrelated but real: `gvactor.cpp:3006` calls
  `gvShape->SetLightsOut(true)` for DESTROYED/DISABLED with **no `else` branch**
  to clear it, unlike `mech3d.cpp:3968-3977` which sets both arms. Once latched,
  a vehicle's lightsOut never clears. Not the reported bug (the trucks are
  intact) but worth a separate slice.

## Verification performed

- Harness `tools/visual_verify/capture.py`, install `mc2-win64-v0.3`, mc2_10
  saved bookmarks, trigger frame 431, settle 30, fixed timestep.
  **All arms `engine_capture_fired=True`, `deterministic=True` (2/2 poses).**
- Gate reached the engine: `gate_env` in the ON capture manifests contains
  `MC2_LEGACY_LIT_MC2_FRAME` on both poses. (`gate_status` reports
  ENV_DROP_RISK for `scripts/run_smoke.py` — that allowlist was **not** edited
  because `run_smoke.py` is claimed by lane LIGHT-REALISM-1. It does not affect
  this harness, but a tier1 gate-ON smoke would silently run gate-OFF.)
- Build/deploy attribution: exe and shader md5 **match** worktree vs install
  (`3ffab36e92cf78ae2d6cb410cca1f87b` exe, `2d193ad029b11e260c8b391a9735819b`
  shader); manifest `src_commit=b01a7a965223`; HEAD confirmed still `b01a7a96`
  after the build. The exaggeration patch was applied to the **installed** shader
  only and has been reverted (md5 re-verified above).
- **tier1 NOT run** — the change is default-OFF and inert, and the smoke lane was
  contended. Outstanding.
- **DAY not checked** — moot while the fix is unproven; required before this gate
  is ever considered for default-ON.

## Residual / open

1. The reported bug is **unfixed**. Attribution is still open.
2. `MC2_LEGACY_LIT_MC2_FRAME` is a correct-looking frame fix with no proven
   visual benefit yet. It should **stay default-OFF** until someone grades what
   it does to the small set of shapes that program actually draws (and to day).
3. tier1 5/5 not run.
4. Unreconciled: foliage recolour in the ON arm vs no magenta in the EXAG arm.
5. Lane coordination held: **no** edits to `gvactor.cpp`, `tgl.cpp`,
   `gos_terrain_lighting.cpp`, `gos_static_prop_batcher.cpp` or `run_smoke.py`
   (all claimed by GHOST-LIGHT-VISIBILITY-1 / LIGHT-BILLBOARD-GPU-1 /
   LIGHT-REALISM-1). Files touched: `shaders/gos_tex_vertex_lighted.vert`,
   `mclib/txmmgr.cpp`, `RenderCore/RendererFeatureRegistry.h`,
   `docs/tier1_env_vars.md`.

## Evidence paths

- `scratch/vehnightlit/OFF2/` — gate OFF, poses 204753 + 212019
- `scratch/vehnightlit/ON/` — gate ON, same poses, same exe
- `scratch/vehnightlit/EXAG/` — magenta exaggeration probe, 204753
- `scratch/vehnightlit/zoom_204753_{OFF,ON,EXAG}.png` — 3x crops of the compound
- `release_assets/0.5.2/REVIEW/vehicle_night_lit_1/` — REVIEW pack copy
