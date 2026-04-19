# GPU Static Prop Renderer — Design

Date: 2026-04-19
Worktree: `nifty-mendeleev`
Author: Joe + Claude (Opus 4.7)

## Problem

Static props (buildings, trees, walls, towers, wrecks, generic props) disappear
in groups at certain camera/zoom combinations, especially at wolfman altitude
and when paused. Prior investigation proved three independent but tangled root
causes:

1. **Broken per-object angular frustum cull.** `BldgAppearance::recalcBounds()`
   and siblings use `cameraFrame.trans_to_frame` with
   `verticalSphereClipConstant` / `horizontalSphereClipConstant`. That geometry
   does not hold at steep wolfman-pitch angles — measured false-negative rate
   at wolfman zoom is ~87% (`inViewFalse=2315` of `2642`).
2. **`update()` ↔ `recalcBounds()` coupling.** `*Appearance::update()` gates
   `TransformMultiShape()` behind `if (inView)`. Once `recalcBounds()` sets
   `inView=false`, the shape's `shapeToWorld` is stale for the rest of the
   frame, and force-enabling render later produces garbage MLR clipping.
3. **MLR `uint16` vertex-index ceiling.** `Max_Number_Vertices_Per_Frame +
   4*Max_Number_ScreenQuads < 65536` is a hard architectural cap.
   `MLRVertexLimitReached` flips mid-frame and silently gates all subsequent
   `bldgShape->Render()` calls, producing a planar visibility cutoff in
   iteration order.

Every attempted fix collided with at least one of the other two: relaxing the
cull drops more work on MLR and trips its vertex ceiling; widening MLR past
65K overflows the index type; correcting the cull cascades through the
`update()` gate. See the session summary in CLAUDE.md and
`memory/clip_w_sign_trap.md` for the full walk.

## Goal

Replace the CPU-side static-prop submission pipeline with a GPU-residency
renderer that submits every static prop every frame and lets the GPU clipper
handle visibility. Eliminate the MLR ceiling and the per-object cull for this
render class while preserving MC2's vertex-lighting semantics bit-for-bit.

## Non-goals

- Mechs (stay on MLR; `Mech3DAppearance` is out of scope).
- gosFX particle system.
- Skinned meshes of any kind.
- Shader-evaluated world lights. CPU vertex lighting via `TransformMultiShape`
  + `multiSetLightList` is retained for now; moving lighting to the GPU is a
  pure-perf follow-up, not part of this spec.
- Deletion of the old CPU path. It remains behind the killswitch as the
  ground-truth visual reference for validation and rollback.

## Render-contract classification

Per `docs/render-contract.md`, the new path is a **Bucket A (world-space
authoritative)** entry.

- **Authoritative submission space.** Raw MC2 world space. Local-space
  `gos_VERTEX` positions are transformed by a per-instance `modelMatrix` in
  the vertex shader.
- **Projection owner.** GPU. Shader computes
  `gl_Position = worldToClip * modelMatrix * vec4(local.xyz, 1)` using the
  same `worldToClip` matrix as terrain.
- **Visibility owner.** GPU clip-space (GL clipper). No CPU frustum cull,
  no `projectZ()` dependency, no `pz` gate. This is explicitly **C2** from
  brainstorming — render every static prop, let the GPU cull.
- **Shadow owner.** Forward depth pass inside the batcher itself (a
  depth-only draw that reuses the shared type VBO + per-instance SSBO against
  the dynamic shadow FBO). This aligns with the contract's D3 target state of
  shrinking post-process shadowing for world geometry.
- **Bridge state.** The killswitch branch in each `*Appearance::render()`
  (old CPU path when `g_useGpuStaticProps=false`) is a temporary Bucket D
  bridge. Exit plan: once the new path is validated across all five
  appearance types, delete the old branch and retire `mcTextureManager`
  usage for static props.

## Architecture

A new subsystem, `GpuStaticPropBatcher`, owns three GL objects.

### 1. Shared type VBO/IBO (persistent per map, truly immutable)

Built once at map load and **never modified afterwards**. The VBO/IBO must be
immutable for the map's lifetime — runtime reallocation is explicitly
prohibited by the design, not merely avoided.

For every static-prop actor spawned into the current map, the registration
pass walks its `TG_TypeMultiShape` AND every `TG_TypeMultiShape*` it can
mutate into during the match:

- undamaged variant
- damaged variant (buildings)
- destroyed variant (buildings)
- wreck variant (GVAppearance)
- any alternate mesh declared in the actor's type data

All variants are appended to the shared VBO/IBO in one pass. In MC2, these
variants are declared at spawn time from the actor's type data; they are not
discovered at runtime. The registration enumeration is the single source of
truth.

- **Indices:** `uint32_t`. Breaks the MLR 65K ceiling permanently.
- **Per-type descriptor:** `{firstIndex, indexCount, baseVertex, textureSet[]}`.
- **Buffer storage:** `glBufferStorage` with flags `0` (fully immutable,
  GPU-only). Explicitly rejects late writes.
- **Expected size:** ≤500k vertices × 40B ≈ 20 MB VBO + a few MB IBO. Well
  under our headroom (1.3 GB VRAM used of available 24 GB on RX 7900 XTX).

**Layer B safety net.** If `submit()` is ever called with a `TG_TypeShape*`
not present in the registration table, that single actor falls back to the
old CPU path (`shape->Render(...)`) for that frame, and the batcher logs a
single `[GPUPROPS] unregistered type <name>` warning per unique unseen type.
No mid-frame reallocation. If the safety net fires in practice, it is a bug
in the Layer A enumeration — the fix is to correct the enumeration, not to
grow the buffer.

### 2. Per-instance SSBO (rewritten per frame)

One entry per submitted static-prop actor:

```
struct GpuStaticPropInstance {
    mat4  modelMatrix;       // shape-to-world
    uint  typeID;            // index into type descriptor table
    uint  firstColorOffset;  // into per-instance color buffer (see 3)
    uint  flags;             // bit 0: alphaTest, bit 1: lightsOut,
                             //   bit 2: isWindow, bit 3: isSpotlight
    uint  pad;
    vec4  aRGBHighlight;     // additive highlight (damage, selection, etc.)
    vec4  fogRGB;            // per-instance fog override
};
```

Expected size: ~100 B × ~3000 instances ≈ 300 KB per frame.

**Upload path.** Persistent coherent mapped ring of 3 buffers, one per
in-flight frame:

```
glBufferStorage(GL_SHADER_STORAGE_BUFFER, 3 * sizePerFrame, nullptr,
                GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
void* mapped = glMapBufferRange(..., 0, 3 * sizePerFrame,
                GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
```

The CPU writes into frame N's slice while the GPU reads frame N-1 / N-2.
A fence (`glFenceSync`) per frame gates reuse of the slot on wrap-around.
This eliminates the `glBufferSubData` driver-synchronization cost entirely.

**Fallback.** If persistent mapping returns `NULL` on an unexpected driver,
fall back to `glMapBufferRange(GL_MAP_WRITE_BIT |
GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT)` per frame with the
same ring buffer. `ARB_buffer_storage` is guaranteed on RX 7900 XTX so this
fallback is defensive, not expected.

### 3. Per-instance vertex-color stream (rewritten per frame)

The existing `TransformMultiShape` / `multiSetLightList` CPU path continues to
bake per-vertex ARGB lighting into each `TG_Shape::listOfColors`. Instead of
those colors feeding `mcTextureManager->addVertices`, the batcher memcpys them
into a tightly packed per-instance color buffer.

- **Layout:** `[instance0_verts][instance1_verts]...`, each entry a
  `uint32_t` ARGB.
- **Indexed in the vertex shader** by `gl_VertexID + firstColorOffset[instID]`
  (stored in the SSBO entry).
- **Expected size:** avg 200 verts × 4B × 3000 instances ≈ 2.4 MB per frame
  (~144 MB/s at 60 FPS — negligible on PCIe 4.0 x16).
- **Upload path:** same persistent coherent mapped ring as the instance SSBO.

## Data flow per frame

```
game tick
  └─ *Appearance::update() (CPU, unchanged)
       └─ TransformMultiShape()  ← bakes listOfColors
          (always called on the batcher path; the old
           `if (inView)` gate is bypassed)

render phase
  └─ *Appearance::render()
       └─ if (g_useGpuStaticProps):
            batcher.submit(shape, shapeToWorld)
              ├─ append to CPU-side staging instance list
              └─ memcpy shape->listOfColors into
                 per-instance color staging buffer
          else:
            shape->Render(...)  ← old path, unchanged

(after all static-prop render() calls)
  └─ batcher.flush()
       ├─ glBufferSubData instance SSBO (ring buffer)
       ├─ glBufferSubData color buffer  (ring buffer)
       └─ for each (typeID, textureSet) group:
            bind textures (T1 — via existing mcTextureManager handles)
            glDrawElementsInstancedBaseVertex(
              GL_TRIANGLES,
              type.indexCount,
              GL_UNSIGNED_INT,
              type.firstIndex,
              instanceCount,
              type.baseVertex)

shadow phase
  └─ batcher.flushShadow()  ← same buffers, depth-only shader,
                              into dynamic shadow FBO
```

## Shaders

Two new programs.

### `static_prop.vert` / `static_prop.frag`

- Reads `gos_VERTEX` attributes from the type VBO (position, normal, UV).
- Reads `modelMatrix`, flags, highlight, fog from SSBO indexed by
  `gl_InstanceID`.
- Reads baked ARGB from the per-instance color buffer indexed by
  `gl_VertexID + firstColorOffset`.
- Computes `gl_Position = worldToClip * modelMatrix * vec4(pos, 1)`.
- Fragment: samples type's texture set, multiplies by baked ARGB, adds
  `aRGBHighlight`, applies fog using the same formula as the non-overlay path
  in `gos_tex_vertex.frag` (`mix(fog_color.rgb, c.rgb, FogValue)`) — avoids
  the reversed-parameter trap documented in the 2026-04-14 CLAUDE.md note.
- Optional alpha test when `flags & 1`.

### `static_prop_shadow.vert` / `static_prop_shadow.frag`

- Same vertex transform, but with `lightSpaceMVP` in place of `worldToClip`.
- Depth-only, writes to the existing dynamic shadow FBO.
- Replaces the `g_shadowShapes` CPU collector for static props only; mech
  shadows continue to use the old collector path.

### Shadow-flush GL state contract

`batcher.flushShadow()` must save, set, and restore the following GL state
explicitly to avoid cross-talk with the existing mech-shadow collector that
runs in the same `Shadow.DynPass`:

- `GL_DEPTH_WRITEMASK` — force `GL_TRUE` after shader bind (works around the
  `gosRenderMaterial::apply()` override documented in memory).
- `GL_CULL_FACE_MODE` — match the existing object-shadow shader's convention
  (back-face cull).
- `GL_DEPTH_TEST` — enabled.
- `GL_BLEND` — disabled for depth-only.

Save with `glGetIntegerv` before the draw; restore after. Do not assume
the incoming state — mech-shadow draws run immediately before/after and
may have set any of these.

### Standard MC2 shader conventions

- No `#version` in the shader files; passed as prefix to `makeProgram()`.
- `apply()` discipline on any deferred uniforms. Direct-uploaded matrices
  use `GL_FALSE` transpose.

## Culling — C2 (render everything)

Zero per-object CPU cull in the new path. `*Appearance::render()` calls
`batcher.submit()` unconditionally for every static prop actor. The GPU
clipper handles visibility.

`recalcBounds()` is still called each frame because it's responsible for
producing `shapeToWorld`, which the batcher reads. Its `inView` output is
ignored by the new path, and `update()`'s `if (inView) TransformMultiShape()`
gate is bypassed when the killswitch is on so that the lit color stream is
always fresh.

This is exactly the behavior the bug-hunting session attempted to achieve in
Attempt 6 (projectZ cull in `recalcBounds`) without the cascading failure mode
— because the new path doesn't depend on `inView` at all.

## Killswitch — K1

Global `bool g_useGpuStaticProps` defined in `code/mechcmd2.cpp`, default
`false` initially. Debug hotkey (proposed RAlt+8) toggles live.

Each of the five `*Appearance::render()` sites gets a one-line branch:

```cpp
if (g_useGpuStaticProps) {
    GpuStaticPropBatcher::instance().submit(
        bldgShape, shapeToWorld, alphaTest, highlight, fog);
} else {
    bldgShape->Render(forceZ, false, alphaValue, false,
                      &shapeToClip, &shapeToWorld);
}
```

Shadow path in `mclib/tgl.cpp` gets the equivalent branch: when the killswitch
is on, static-prop shapes are not added to `g_shadowShapes`, and the batcher's
shadow flush runs inside `Shadow.DynPass` instead.

## Migration — M1

1. **Buildings.** Land `GpuStaticPropBatcher`, both shader programs,
   killswitch wiring in `BldgAppearance`, and the shadow-path branch in
   `tgl.cpp` / `txmmgr.cpp`. Validate in-game at wolfman zoom, standard RTS
   zoom, and when paused. Visual diff with killswitch toggled.
2. **Trees.** Wire `TreeAppearance::render()`. Alpha-test leaves are the
   main risk; validate foliage rendering specifically.
3. **Generic / GV / plane fallback.** Wire `GenericAppearance`,
   `GVAppearance`, and the `appear.cpp` plane mesh fallback in one pass —
   all three share `TG_Shape::Render` mechanics so any issue caught in steps
   1–2 already applies.
4. **(Out of scope.)** Future follow-up: remove the old CPU branch, delete
   the `if (g_useGpuStaticProps)` bridge, retire `mcTextureManager` static-
   prop submission.

## Files

### New

- `GameOS/gameos/gos_static_prop_batcher.cpp`
- `GameOS/gameos/gos_static_prop_batcher.h`
- `shaders/static_prop.vert`
- `shaders/static_prop.frag`
- `shaders/static_prop_shadow.vert`
- `shaders/static_prop_shadow.frag`

### Modified

- `mclib/bdactor.cpp` — killswitch branch in `BldgAppearance::render()`.
- `mclib/treeactor.cpp` — killswitch branch in `TreeAppearance::render()`.
- `mclib/genactor.cpp` — killswitch branch in `GenericAppearance::render()`.
- `mclib/gvactor.cpp` — killswitch branch in `GVAppearance::render()`.
- `mclib/appear.cpp` — killswitch branch in the plane fallback render path.
- `mclib/tgl.cpp` — skip `g_shadowShapes` add for static props when
  killswitch is on.
- `mclib/txmmgr.cpp` — call `batcher.flushShadow()` inside `Shadow.DynPass`.
- `GameOS/gameos/gameos_graphics.cpp` — call `batcher.flush()` at end of
  object render phase; own batcher lifetime.
- `GameOS/gameos/gameosmain.cpp` — construct/destroy batcher around map
  load/unload.
- `code/mechcmd2.cpp` — `g_useGpuStaticProps` definition, RAlt+8 debug
  hotkey binding.
- `GameOS/gameos/CMakeLists.txt` — new source files.

Reference docs that should be read alongside this spec:

- `docs/render-contract.md` — authoritative render-path contract.
- `docs/architecture.md` — render pipeline, coordinate spaces, render order.
- `docs/amd-driver-rules.md` — AMD RX 7900 XTX quirks relevant to SSBO and
  instanced draws.
- `memory/clip_w_sign_trap.md` — do not use `sign(clip.w)` for front/back.
- `memory/terrain_tes_projection.md` — `worldToClip` math conventions.

## Risks and open questions

1. **Vertex-color buffer sync race.** `TransformMultiShape()` runs per actor
   during `update()`, but `submit()` reads `listOfColors` during `render()`.
   This is the same ordering MC2 already relies on — no new race — but the
   batcher path depends on it explicitly. Document the invariant in the
   batcher header.
2. **Paint / damage mesh swap.** When a building is destroyed, `BldgAppearance`
   swaps to a different `TG_MultiShape`. The batcher resolves `typeID` per
   frame from the shape pointer (pointer lookup in a flat hash). Mid-game
   VBO reallocation is prohibited by design — see "Layer B safety net" in
   the shared VBO section. Correctness depends on the map-load registration
   pass enumerating every variant mesh any actor can swap into.
3. **Alpha testing vs. alpha blending.** The new fragment shader handles
   alpha-test via `discard` when `flags & 1`, which is correct for unordered
   instanced draws. However, true alpha *blending* (translucent materials)
   produces severe order artifacts without back-to-front sorting — which the
   C2 "render everything, any order" design does not provide. **M1 gate:**
   before enabling a given appearance type on the new path, audit its
   `TG_Shape` materials for `GL_BLEND` usage. If any are found, that type
   stays on the CPU path and a separate sorted transparent batch is
   designed in a follow-up spec. Expected outcome: static props use only
   alpha-test (tree leaves, building windows); true blending is reserved
   for gosFX and water, both out of scope.
4. **`Shadow.DynPass` FBO ordering.** The batcher's shadow flush runs inside
   the existing dynamic shadow FBO, interleaved with mech-shadow draws. GL
   state hazards are handled by the explicit save/restore contract in the
   "Shadow-flush GL state contract" section, not implicitly.
5. **Per-frame buffer sizing.** 3000 instances is an observed figure, not a
   guaranteed cap. Batcher should use a grow-only ring buffer and re-allocate
   if a frame exceeds capacity (once, logged, never shrinks).
6. **Textures per type.** The T1 batch granularity is `(typeID, textureSet)`.
   Some `TG_TypeShape`s have multiple textures across different sub-shapes.
   Assume worst case ~3 draws per type × ~100 types = 300 draws per frame;
   if profiling shows this matters, texture-handle array + draw-indirect is
   a future optimization (not part of this spec).

## Test plan

- Build `--config RelWithDebInfo` via `/mc2-build`.
- Deploy via `/mc2-deploy` with per-file `diff -q` verification.
- In-game validation (both killswitch states, toggle via RAlt+8):
  1. Standard RTS zoom, pan around a building-heavy map. No disappearances.
  2. Wolfman zoom (altitude 6000, `GameVisibleVertices=200`). No
     disappearances in the ~200-tile visible radius.
  3. Pause and pan. No disappearances while paused.
  4. Destroy a building. Verify mesh swap renders correctly.
  5. Dynamic shadows from buildings land correctly on terrain.
  6. Profile in Tracy: `GpuStaticProps.Submit` and `GpuStaticProps.Flush`
     zones on CPU, `GpuStaticProps.Draw` on GPU. Compare against old-path
     `Render.3DObjects` zone.
- Per migration step (M1), re-run the test plan with only that step's
  appearance type enabled on the new path.
