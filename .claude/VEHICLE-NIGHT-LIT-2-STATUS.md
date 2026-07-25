# VEHICLE-NIGHT-LIT-2 — STATUS

**Verdict: reported defect still NOT FIXED — but attribution moved decisively,
and VEHICLE-NIGHT-LIT-1's premise is OVERTURNED.**

The lane's mandated job was "NAME the program that paints the truck, confirm it,
then fix". Naming was done with RenderDoc plus four independent exaggeration
probes. The result is a set of hard eliminations plus one hard positive, and it
points at a root-cause class nobody has considered yet. The in-scope latent
`SetLightsOut` bug is fixed and shipped.

- Worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`
- Commit: **`7c059585`** — `fix(vehicles): VEHICLE-NIGHT-LIT-2 — clear GV lightsOut latch (gated, default-OFF)`
- Gate: **`MC2_GV_LIGHTSOUT_CLEAR`**, default **OFF** (byte-identical)
- Build: **PASS** (`cmake --build build64 --target mc2`, RelWithDebInfo, clean link)
- Install used for probing: `A:/Games/mc2-opengl/mc2-win64-v0.3` (exe `b01a7a96`,
  the VEHICLE-NIGHT-LIT-1 build — probes were shader-only, exe irrelevant)

---

## 1. What RenderDoc says (the mandated attribution)

Capture: `scratch/vehnightlit2/mc2_10_204753d.rdc`, mc2_10 night, saved bookmark
`mc2_10_saved_204753`, 800x600, pose confirmed identical to the visual_verify
PNG. **All attribution below is against the pre-composite HDR scene FBO
(`resid 231`), not the backbuffer**, per the standing warning. `resid 234` (the
second R16G16B16A16_FLOAT target) was checked too and agrees.

Shader identities were established by SPIR-V disassembly, not guessed:

| shader ids | identity | evidence |
|---|---|---|
| VS 226 / PS 227 | **terrain LOD chunk** | entry-point interface `u_mapSide`, `isSkirtFlag`, `v_terrainType` |
| VS 2223 / PS 2224 | **static_prop GPU batcher** (`static_prop.vert`/`.frag`, program `static_prop_coalesce`) | `gl_BaseInstanceARB`, `v_drawID`, `v_objectId`, `v_pbrV1Sun*`; log line `[GPUPROPS-DIAG] static_prop_coalesce program=182` |
| VS 93 / PS 94 | additive glow / light billboards | `One->One Add` blend |
| 258/259, 264/265, 276/277, 279/280 | post-process fullscreen quads | 6 verts, depth off, `DstCol->Zero` and `SrcAlpha->InvSrcAlpha` |

Whole-compound attribution (`region 400,240,300,110`, 256-sample grid,
`scratch/vehnightlit2/compound_attrib.md`):

> **The only geometry programs that contribute any pixel in the entire mc2_10
> compound are the terrain LOD chunk and the static_prop GPU batcher.**
> No `GpuMechBatcher` draw, no `gos_tex_vertex`, no `gos_tex_vertex_lighted`,
> no legacy `masterVertexNodes` array draw appears at all.

Per-pixel history directly on the reported dark truck
(`x 458-483, y 300-307`, located by local-contrast search then confirmed on a
coordinate-gridded 8x zoom, `scratch/vehnightlit2/grid1.png`) returns **terrain
(eid 305) as the sole and final writer**, on both HDR targets.

## 2. The four exaggeration probes (verify-by-exaggeration, as mandated)

Each probe replaced the program's final `FragColor` write with blown-out magenta
`vec4(8,0,8,1)` in the **installed** shader, re-captured the same bookmark, and
was reverted with an md5 re-check afterwards. All four installs verified
byte-identical to the worktree at the end (`mech.frag`, `static_prop.frag`,
`gos_tex_vertex.frag` all `SAME`).

| program | result | reads on the dark truck |
|---|---|---|
| `gos_tex_vertex` | **nothing magenta anywhere** | unchanged |
| `gos_tex_vertex_lighted` | almost nothing (VEHICLE-NIGHT-LIT-1) | unchanged |
| `static_prop` | **compound-wide magenta** — walls, fences, buildings, barrels, huts, tank farm, guard towers, taxiway markers, the small parked trucks in the bottom row | **unchanged — stays dark** |
| `mech.frag` | nothing magenta anywhere | unchanged |

Evidence: `scratch/vehnightlit2/truck_exag_ab.png` (baseline over static_prop-exag,
7x, side by side) is the money shot — the whole frame goes magenta around a truck
that does not.

### Two methodology traps this lane hit — record them, they will bite again

1. **A magenta probe inserted *before* the existing `FragColor = c;` does
   nothing** — the original write still runs and wins. The first `static_prop`
   probe was botched exactly this way and produced a false negative. Always
   *replace* the write, never insert before it. A deliberate-syntax-error run
   (`scratch/vehnightlit2/synerr/`) is the cheap way to prove the engine is
   actually reading your edited file.
2. **`shaders/spv/*.spv` is a SPIR-V package cache that silently defeats source
   edits** for the shaders it covers — `mech`, `postprocess`, `cloud`,
   `edge_fog`, `shoreline`, `ssao`, `hzb_reduce`, `fog_oob`. The first
   `mech.frag` probe was a false negative for this reason. Move the whole `spv/`
   directory aside for the probe run. `static_prop`, `gos_tex_vertex` and the
   terrain shaders are **not** cached, so their probe results are genuine.

## 3. Where this leaves root cause

Converging evidence — RenderDoc pixel history on two HDR targets naming terrain
as the sole writer, plus four object-program exaggerations that all leave those
pixels untouched — supports a root-cause class that no prior lane considered:

> **The reported "dark trucks" are very likely not drawn as lit geometry at all.
> They appear to be part of the terrain surface — baked into the terrain
> colormap / burn-in texture — so they carry a dark day-baked albedo and can
> never receive night object-light, while every genuinely-drawn prop beside them
> (drawn by `static_prop`) lights correctly.**

This is stated as the leading hypothesis, **not as proven**. It is consistent
with everything measured, and it explains why two lanes of shader-lighting work
produced no movement. It is *not* yet confirmed, and the honest counter-evidence
is that the same reasoning implies some vehicle-shaped white objects nearby are
also baked, which has not been independently checked.

**Do not write more shader-lighting code until this is settled.** The next pass
should go straight at the bake: `tools/terrain_gen/burnin_renderer.py`, the
mc2_10 colormap/burn-in sidecars (`mission_facts` / `get_mission_sidecars`), and
whether the dark truck silhouettes are present in the burn-in texture itself.
Sampling that texture is a cheap, decisive, zero-build test.

### What is now DISPROVEN — do not re-run

Carried forward from VEHICLE-NIGHT-LIT-1 plus this lane:

1. Vehicles missing a light slot — ruled out (`GatherLightsParameters` +
   `addLightDataStructure` both run on the vehicle path).
2. `gos_tex_vertex_lighted` frame mismatch is the cause — disproven by A/B and
   by exaggeration (VEHICLE-NIGHT-LIT-1).
3. `gos_tex_vertex_lighted` paints the trucks — disproven (VEHICLE-NIGHT-LIT-1).
4. **The legacy CPU triangle queue (`gos_tex_vertex`) paints the trucks** —
   disproven here. It paints nothing in this scene.
5. **The static-prop batcher paints the trucks** — disproven here. It paints
   everything *except* them. (This was VEHICLE-NIGHT-LIT-1's recommended
   starting hypothesis; it is dead.)
6. **The GPU mech batcher (`mech.frag`) paints the trucks** — disproven here.

### A source finding worth keeping regardless

`TG_Shape::Render` / `MultiTransformShape` emit into **two** queues for the same
geometry — the legacy `masterVertexNodes` CPU array queue (per-vertex
`aRGBLight`, drawn by `gos_tex_vertex`, unlit) and the modern `addRenderShape` /
`ShapeRenderer` queue (`gos_tex_vertex_lighted`). Depth compare is `GL_GEQUAL`
(reverse-Z), so **ties pass and the later draw wins**, and `renderLists()` draws
the modern `masterHardwareVertexNodes` batch *first* (txmmgr.cpp:2662) and the
legacy `masterVertexNodes` batch *after* (txmmgr.cpp:3464+). Any shape that
takes both paths therefore ends up displaying the **unlit** legacy result. The
CPU vertex-lighting bake that was supposed to fill `aRGBLight` is dead on the
modern renderer (`tgl.cpp:2154`, `Environment.Renderer != 3`). This is a real
latent hazard of root-cause class (c) and matches
`FAR-BACKDROP-TEXTURED-LIGHT-1`. It is **not** the reported bug — neither queue
paints the trucks — but it is the right thing to look at for any *other*
legacy-shape lighting complaint.

## 4. The latent bug that WAS fixed (in scope, separate commit `7c059585`)

`mclib/gvactor.cpp` `GVAppearance::update` called
`gvShape->SetLightsOut(true)` for `OBJECT_STATUS_DESTROYED` /
`OBJECT_STATUS_DISABLED` with **no else-branch**, unlike
`mech3d.cpp Mech3DAppearance::update` which sets both arms. The flag was a
one-way latch: once a ground vehicle was disabled its shape could never be lit
again, including after the actor slot is recycled for a live vehicle.

`MC2_GV_LIGHTSOUT_CLEAR=1` adds the missing else-branch. Default-**OFF** keeps
the legacy latch and is byte-identical. Registered in
`RenderCore/RendererFeatureRegistry.h` (`GvLightsOutClear = 49`) and documented
in `docs/tier1_env_vars.md`.

This is **not** the reported defect — the reported trucks are intact and never
disabled.

## 5. Verification — what was and was not done, honestly

- **Build: PASS.** Clean compile + link of `mc2` after the commit.
- **RenderDoc attribution: DONE** (section 1), the lane's headline deliverable.
- **Exaggeration confirmation: DONE** on four programs (section 2).
- **Install integrity: VERIFIED.** All probe edits reverted; installed
  `mech.frag` / `static_prop.frag` / `gos_tex_vertex.frag` byte-compare `SAME`
  against the worktree.
- **OFF-vs-ON A/B at the two night poses: NOT DONE, and deliberately so.** There
  is nothing to A/B — no fix for the reported defect landed. The only shipped
  gate is byte-identical when OFF and changes a latch that this pose never
  exercises.
- **DAY check: NOT DONE**, same reason (still outstanding for
  `MC2_LEGACY_LIT_MC2_FRAME` from lane 1).
- **tier1: NOT RUN.** Deploy to lane `0.3` was attempted and **refused to place
  the exe**: `deploy_payload.py` reports the build fingerprint as
  `MC2_BUILD_GIT_DIRTY=1`, because this shared worktree carries ~189 dirty files
  belonging to four other live lanes (LIGHT-REALISM-1, PROP-SELF-SHADOW-1,
  MC2-03-CEMENT-PINK-1, LETTERBOX-TOP-RESTORE-1). Per
  COMMIT-FIRST-THEN-BUILD-THEN-DEPLOY, a dirty-tree deploy is unattributable, so
  it was not forced. The deployed exe remains `b01a7a96`
  (md5 `3ffab36e...`), which is **not** my commit — recorded here rather than
  papered over. A tier1 run against it would have measured someone else's build.
  **tier1 for `7c059585` is outstanding.**
- **Measurement caveat honoured:** the mission's warning that the mc2_10 night
  A/B rig is unreliable (2-7% pixel drift between two GATE-OFF runs) is why every
  conclusion above rests on **categorical** evidence — "the frame goes magenta
  and this object does not" — rather than on pixel-diff percentages. One early
  pixel-diff in this lane (281k changed pixels on the `gos_tex_vertex` probe)
  was an artifact of comparing against a non-comparable baseline and is
  disregarded; the side-by-side crop against a same-harness baseline shows the
  truck is bit-for-bit unchanged.

## 6. Lane coordination

- Files touched: `mclib/gvactor.cpp`, `RenderCore/RendererFeatureRegistry.h`,
  `docs/tier1_env_vars.md`. All were clear in `check_conflict`.
- **No edits to `shaders/static_prop.vert`** — contended with the live lane
  PROP-SELF-SHADOW-1. Had this lane's fix landed there it would have needed
  negotiation; it did not, so no negotiation was required. **The next pass should
  expect to contend with PROP-SELF-SHADOW-1 if it goes near the prop path** —
  though on current evidence the fix is likely in the terrain bake, not there.
- **No edits to `scripts/run_smoke.py`** (LIGHT-REALISM-1),
  `GameOS/gameos/gos_terrain_lighting.cpp` (LIGHT-REALISM-1),
  `code/gamecam.cpp` (LETTERBOX-TOP-RESTORE-1),
  `shaders/terrain_lod_chunk.frag` (MC2-03-CEMENT-PINK-1).
- Coordination note for LIGHT-REALISM-1: it is raising the 64-light cap. On this
  lane's evidence the trucks are **not** losing light slots — they are not being
  drawn as lit objects at all — so the cap raise is not expected to fix them.
- Housekeeping: 8 `mech.*.spv` files were removed from the install's
  `shaders/spv/` during probing. That directory is a regenerating cache; the
  engine falls back to source compile and rebuilds them. No action needed.

## 7. Evidence paths

- `scratch/vehnightlit2/mc2_10_204753d.rdc` — the capture everything is attributed against
- `scratch/vehnightlit2/compound_attrib.md` — 256-sample whole-compound attribution
- `scratch/vehnightlit2/truck_attrib2.md`, `attr234.md`, `px_470_302.md` — per-pixel history on the truck
- `scratch/vehnightlit2/dis_{vs,ps}305.md`, `dis_ps5780.md` — SPIR-V disassembly naming the programs
- `scratch/vehnightlit2/truck_exag_ab.png` — **baseline vs static_prop-exag at the truck (the decisive image)**
- `scratch/vehnightlit2/exag_sp2_compound.png` — compound-wide static_prop magenta
- `scratch/vehnightlit2/truck_gtv_ab.png` — `gos_tex_vertex` probe, no change
- `scratch/vehnightlit2/exag_mech2_truck.png` — `mech.frag` probe (spv cache off), no change
- `scratch/vehnightlit2/grid1.png` — coordinate-gridded zoom used to pin the truck's pixels
- `scratch/vehnightlit2/synerr/` — syntax-error run proving the engine reads the edited shader
- `release_assets/0.5.2/REVIEW/vehicle_night_lit_1/` — REVIEW pack copy
