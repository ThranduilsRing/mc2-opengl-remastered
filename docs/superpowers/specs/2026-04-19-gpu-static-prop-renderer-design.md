# GPU Static Prop Renderer — Design

Date: 2026-04-19
Worktree: `nifty-mendeleev`
Author: ThranduilsRing + Claude (Opus 4.7)

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

## AMD invariants (hard requirements)

Per `docs/amd-driver-rules.md` and project scar tissue, the RX 7900 XTX driver
silently drops draws or rasterizes incorrectly if any of these are violated.
The batcher and both shader programs must respect all of them.

1. **Attribute 0 enabled on every VAO.** Position lives at
   `layout(location = 0)` in both `static_prop.vert` and
   `static_prop_shadow.vert`, and the VAO enables that attribute. A VAO with
   no attribute 0 bound may cause AMD to skip draws entirely.
2. **`gl_FragDepth` explicitly written in the shadow fragment shader.**
   `static_prop_shadow.frag` contains `gl_FragDepth = gl_FragCoord.z;`.
   Depth-only fragment shaders without this write are a known AMD failure
   mode.
3. **Dummy color attachment on the shadow FBO during static-prop draws.**
   Depth-only FBOs with no color attachment are unreliable on AMD. The
   existing `Shadow.DynPass` FBO must have a (potentially 1×1 or reused)
   color attachment bound for the static-prop shadow draw, even though no
   color is written.
4. **Unbind shadow texture from sampler unit 9 before rendering into the
   shadow FBO.** The shadow map is sampled at unit 9 by the main terrain
   and object shaders. If it remains bound for sampling while the shadow
   FBO is being rendered into, AMD may refuse to rasterize (feedback-loop
   hazard). Batcher unbinds unit 9 before `flushShadow()` and rebinds after.
5. **Per-batch program/material re-apply and direct-uniform re-upload.**
   Any engine helper may call `material->end()` or `apply()` internally.
   The batcher treats every draw batch as requiring: bind program → set
   deferred uniforms → `apply()` → set direct uniforms → draw. No cross-batch
   state is assumed.
6. **Matrix transpose discipline, no mixing within a program.** For this
   design, all matrix uploads on both programs are **direct**
   (`glUniformMatrix4fv` post-`apply()`), all use `GL_FALSE` transpose
   (row-major source data uploaded directly). The deferred path with
   `GL_TRUE` is not used by either program — mixing the two on the same
   program is banned by the rules doc and avoided here by construction.
7. **No `sampler2DArray`.** Textures bind to individual `sampler2D` units.
   This is consistent with T1 batching (per-packet texture bind).

## Architecture

A new subsystem, `GpuStaticPropBatcher`, owns three GL objects.

### 1. Shared geometry VBO/IBO (persistent per map, truly immutable)

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

All variants are appended to the shared VBO/IBO in one pass.

**Vertex layout on the shared VBO** (all attributes required, position
MUST be location 0 per AMD invariant 1):

```
layout(location = 0) in vec3  a_position;       // local-space position
layout(location = 1) in vec3  a_normal;
layout(location = 2) in vec2  a_uv;
layout(location = 3) in uint  a_localVertexID;  // shape-scope: 0..N-1
                                                // across the owning type's
                                                // full vertex block
                                                // (NOT packet-scope)
```

`a_localVertexID` is numbered **per owning type**, not per packet. For a
type with N vertices split across three packets, the first packet's
vertices carry `a_localVertexID` values in the subrange it occupies, the
second packet's in the next subrange, and so on — all values are unique
within `[0, N)` across the type. This is deliberate: the per-instance
color block is baked at TG_Shape granularity with N entries, so every
packet of that type reads the same shared color block via
`instance.firstColorOffset + a_localVertexID`.

Written at registration time so the fragment/vertex shader can index
per-instance color streams without relying on `gl_VertexID` (which
includes the base-vertex offset under
`glDrawElementsInstancedBaseVertex` and would produce driver-dependent
garbage). ~4 B × 500k vertices = 2 MB extra, which is noise.

**Geometry table layout — draw packets, not types.**

A single `TG_TypeMultiShape` typically contains multiple sub-shapes with
different texture bindings and possibly mixed opaque/alpha-test ranges. The
old path iterates these as `numNodes` and binds per-node. The new path
mirrors that granularity.

The immutable geometry table stores **draw packets**:

```
struct GpuStaticPropPacket {
    uint32_t firstIndex;     // into shared IBO
    uint32_t indexCount;
    int32_t  baseVertex;     // into shared VBO
    uint32_t textureHandle;  // mcTextureManager GL handle
    uint32_t materialFlags;  // bit 0: alphaTest, bit 1: twoSided, ...
};
```

A `typeID` resolves to a contiguous range of packet IDs
(`{firstPacket, packetCount}`). Instancing happens **per packet**, not per
type: for each packet, the batcher groups all instances of its owning type
and issues one `glDrawElementsInstancedBaseVertex`.

**Packet enumeration order is deterministic and matches the old path.**
Packets within a type are written in the same order as
`TG_TypeMultiShape::listOfNodes[0..numNodes-1]`. This is required, not
incidental:

- z-ties between sub-shapes of a type render in the same order as the old
  `TG_MultiShape::Render()` loop.
- alpha-test edges fall on the same triangles.
- any latent state dependency in the old path (texture bind sequence,
  per-node state leakage) reproduces identically.

Making packet order match `numNodes` order collapses a whole class of
"looks slightly different after the port" debugging into equality checks.

- **Indices:** `uint32_t`. Breaks the MLR 65K ceiling permanently.
- **Per-type descriptor:** `{firstPacket, packetCount}` + model-matrix ownership.
- **Buffer storage:** `glBufferStorage` with flags `0` (fully immutable,
  GPU-only). Explicitly rejects late writes.
- **Expected size:** ≤500k vertices × 44B ≈ 22 MB VBO + a few MB IBO +
  ~20 KB packet table. Well under our headroom (1.3 GB VRAM used of
  available 24 GB on RX 7900 XTX).

**Layer B safety net.** If `submit()` is ever called with a `TG_TypeShape*`
not present in the registration table, that single actor falls back to the
old CPU path (`shape->Render(...)`) for that frame, and the batcher logs a
single `[GPUPROPS] unregistered type <name>` warning per unique unseen type.
No mid-frame reallocation. If the safety net fires in practice, it is a bug
in the Layer A enumeration — the fix is to correct the enumeration, not to
grow the buffer.

### 2. Per-instance SSBO (rewritten per frame)

One entry per submitted static-prop actor:

```glsl
// GLSL, std430
layout(std430, binding = 0) readonly buffer Instances {
    GpuStaticPropInstance instances[];
};
struct GpuStaticPropInstance {
    mat4  modelMatrix;       // shape-to-world, row-major source
    uint  typeID;            // index into type descriptor table
    uint  firstColorOffset;  // into per-instance color buffer (see 3)
    uint  flags;             // bit 0: lightsOut, bit 1: isWindow,
                             //   bit 2: isSpotlight
                             // alphaTest lives on the packet, not the instance.
    uint  _pad0;
    vec4  aRGBHighlight;     // additive highlight (damage, selection, etc.)
    vec4  fogRGB;            // per-instance fog override
};
```

**CPU/GPU layout contract.** Applies to every shader-visible struct — the
instance SSBO entry AND the packet descriptor table (if exposed to the GPU
as an SSBO rather than consumed only CPU-side).

Required on the C++ side:

- explicit `alignas(16)` on any struct used in an `std430` buffer
- fixed-width types (`uint32_t`, `int32_t`, `float`, `glm::vec4`,
  `glm::mat4`) — no `size_t`, no `int`, no `bool`
- `static_assert(sizeof(...))` against the expected GLSL size
- `static_assert(offsetof(...))` against the expected GLSL offset for
  **every** shader-visible field
- shader programs declare the matching struct with explicit `layout(std430)`
  on the buffer, never `std140`

These asserts live in `gos_static_prop_batcher.h` and fail the build on
mismatch. Prevents the "someone refactors the struct six months later and
only AMD explodes" failure mode, and also catches alignment drift from
packing changes on the GLSL side.

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
- **Indexed in the vertex shader** by
  `colorBuffer[instance.firstColorOffset + a_localVertexID]`. Does NOT use
  `gl_VertexID` — that value includes `baseVertex` under
  `glDrawElementsInstancedBaseVertex` and its exact behavior varies by
  driver/shader version. The explicit `a_localVertexID` attribute is the
  stable address.
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
       ├─ advance ring-buffer slot N (fence-gated)
       ├─ memcpy staged instances → persistent-mapped instance SSBO slot N
       ├─ memcpy staged colors    → persistent-mapped color SSBO slot N
       │   (coherent map; no glBufferSubData anywhere)
       ├─ bind VAO, bind program, set deferred uniforms, apply(),
       │   set direct uniforms
       └─ for each packet in per-packet work list:
            bind packet.textureHandle on the packet's sampler unit
            glDrawElementsInstancedBaseVertex(
              GL_TRIANGLES,
              packet.indexCount,
              GL_UNSIGNED_INT,
              packet.firstIndex,
              instanceCountForOwningType,
              packet.baseVertex)
       — Invariant: every packet belonging to a given type is drawn against
         the SAME contiguous slice of instance SSBO entries, in the SAME
         order. The batcher writes each type's instance block once and
         reuses the same `[firstInstance, instanceCount)` range across all
         of that type's packets. This is a core correctness assumption:
         it mirrors the old `TG_MultiShape::Render()` inner loop (outer:
         instance, inner: node/packet) and makes packet traversal match
         the `numNodes` order pixel-for-pixel.

shadow phase
  └─ batcher.flushShadow()  ← same buffers, depth-only shader,
                              into dynamic shadow FBO
                              (see "Shadow-flush GL state contract")
```

Per AMD invariant 5, the bind/apply/direct-uniforms sequence is re-run at the
start of `flush()` and again at the start of `flushShadow()`. It is **not**
assumed to survive across an intervening engine helper that may call
`material->end()`.

## Shaders

Two new programs.

### `static_prop.vert` / `static_prop.frag`

- Reads `gos_VERTEX` attributes from the type VBO (position, normal, UV).
- Reads `modelMatrix`, flags, highlight, fog from SSBO indexed by
  `gl_InstanceID`.
- Reads baked ARGB from the per-instance color buffer indexed by
  `instance.firstColorOffset + a_localVertexID`. Does not use `gl_VertexID`.
- Computes `gl_Position = worldToClip * modelMatrix * vec4(pos, 1)`.
- Fragment: samples packet's texture, multiplies by baked ARGB, adds
  `aRGBHighlight`, applies fog using the same formula as the non-overlay path
  in `gos_tex_vertex.frag` (`mix(fog_color.rgb, c.rgb, FogValue)`) — avoids
  the reversed-parameter trap documented in the 2026-04-14 CLAUDE.md note.
- Alpha test: when `packet.materialFlags & ALPHA_TEST_BIT`, `discard` when
  `tex_color.a < 0.5`. This matches the primary static-prop convention in
  `gos_tex_vertex.frag`. The `== 0.5` convention in
  `gos_tex_vertex_lighted.frag` is specific to the projected-overlay branch
  and is not used here.

### `static_prop_shadow.vert` / `static_prop_shadow.frag`

- Same vertex transform, but with `lightSpaceMVP` in place of `worldToClip`.
- Depth-only, writes to the existing dynamic shadow FBO.
- Replaces the `g_shadowShapes` CPU collector for static props only; mech
  shadows continue to use the old collector path.

### Shadow-flush GL state contract

`batcher.flushShadow()` **owns** the GL state it needs — no `glGet*` query,
no save/restore dance. State is set explicitly every call; the next pass
(mech shadow collector, terrain draw, etc.) is responsible for resetting
what it needs. This matches how `gameos_graphics.cpp` already manages
state for terrain and shadow draws.

Explicit state before each shadow draw:

- `glDepthMask(GL_TRUE)` — forced after shader bind (works around the
  `gosRenderMaterial::apply()` `glDepthMask` override documented in memory).
- `glCullFace(GL_BACK)` — matches the existing object-shadow shader convention.
- `glEnable(GL_DEPTH_TEST)`.
- `glDisable(GL_BLEND)`.
- Unbind sampler unit 9 (`glActiveTexture(GL_TEXTURE9); glBindTexture(GL_TEXTURE_2D, 0);`)
  per AMD invariant 4 — the shadow map cannot be bound for sampling while
  the shadow FBO is being rendered into. Rebind after the flush.
- Confirm the `Shadow.DynPass` FBO has a color attachment bound per AMD
  invariant 3. If the existing FBO is depth-only, the batcher attaches a
  reused 1×1 color renderbuffer before its first draw and detaches after.

Shader requirements:

- Position at `layout(location = 0)` (AMD invariant 1).
- `gl_FragDepth = gl_FragCoord.z;` written in `static_prop_shadow.frag`
  (AMD invariant 2).

### Shadow matrix timing

The batcher's shadow pass must read `lightSpaceMVP` (and any other shadow
camera matrices) from the same per-frame snapshot the existing object-shadow
collector uses. The rules doc notes that `draw_screen()` runs before
`gamecam.cpp` sets camera/light values each frame, and the first ~240 frames
after load may see zero camera position.

To avoid introducing a timing divergence with the old path:

- `batcher.flushShadow()` reads matrices from the same globals
  (`Camera::s_lightToWorld`, etc.) that the collector reads, at the same
  point in the frame (inside `Shadow.DynPass`).
- First-frame and post-toggle behavior must match the old path — if the old
  path has bogus shadows for frame 0, the new path should too. Any divergence
  is a bug.
- Validated at M1 step 1 by visual diff on the first 5 frames after map
  load with the killswitch toggled both directions.

### Standard MC2 shader conventions

- No `#version` in the shader files; passed as prefix to `makeProgram()`.
- All matrix uploads on both programs are **direct** (`glUniformMatrix4fv`
  post-`apply()`) and use `GL_FALSE` transpose (row-major source uploaded
  directly). The deferred `GL_TRUE` path is not used on either program.
  Mixing direct and deferred matrix uploads on the same program is banned
  by the rules doc and avoided here by construction (AMD invariant 6).
- Per AMD invariant 5: every draw batch issues bind program → set deferred
  uniforms → `apply()` → set direct uniforms → draw. No direct uniform
  is assumed to survive across an intervening engine helper.

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
`false` initially. Debug hotkey RAlt+0 (free in `gameosmain.cpp` alt_debug
switch — RAlt+8 is taken by terrain debug mode) toggles live.

Each of the five `*Appearance::render()` sites gets a one-line branch:

```cpp
if (g_useGpuStaticProps) {
    GpuStaticPropBatcher::instance().submit(
        bldgShape, shapeToWorld, highlight, fog);
    // Alpha-test, two-sidedness, and other material state live on the
    // packet table (resolved at registration time from TG_TypeShape), not
    // on the submit call. The submit contract carries only per-instance
    // state that can vary frame-to-frame.
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
- `code/mechcmd2.cpp` — `g_useGpuStaticProps` definition, RAlt+0 debug
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
   alpha-test via `discard` gated by `packet.materialFlags & ALPHA_TEST_BIT`
   (packet/material state, not per-instance), which is correct for unordered
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
   the existing dynamic shadow FBO, interleaved with mech-shadow draws.
   All GL state and AMD-specific hazards (sampler unit 9 unbind, dummy color
   attachment, `gl_FragDepth` write, position at location 0) are covered by
   the "Shadow-flush GL state contract" and "AMD invariants" sections.
5. **Shadow matrix timing.** The batcher must consume shadow matrices from
   the same frame-snapshot globals the existing collector reads, at the same
   point in the frame. Addressed in "Shadow matrix timing" section;
   verification is an M1 step 1 test-plan item.
6. **Per-frame buffer sizing.** 3000 instances is an observed figure, not a
   guaranteed cap. Per-frame rings (instance SSBO, color buffer) grow-on-demand
   between frames (re-allocate ring outside any frame, log once, never shrink).
   Map-lifetime geometry buffers do not grow, per the Layer A/B contract.
7. **Packets per type.** The batch granularity is per packet. Expect roughly
   one packet per sub-shape of each `TG_TypeMultiShape`. A worst-case ~3
   packets × ~100 types = 300 draws per frame; negligible. If profiling ever
   shows this matters, texture-handle array + draw-indirect is a future
   optimization (not part of this spec).

## Test plan

- Build `--config RelWithDebInfo` via `/mc2-build`.
- Deploy via `/mc2-deploy` with per-file `diff -q` verification.

### Color-address validation scene (required before visual validation)

Before trusting any visual output from the new path, validate the
`localVertexID` / `firstColorOffset` indexing with a debug render mode.
When `MC2_DEBUG_STATIC_PROP_ADDR=1`, `static_prop.frag` replaces its final
color with a debug value driven by the lookup path:

- Mode A: `out_color = vec3(float(a_localVertexID) / float(maxLocalVertexID));`
  — any shape that renders a smooth 0→1 gradient along its vertex order
  has a correct local index. Any shape that renders discontinuously
  (random bands, wraparound) has a broken address.
- Mode B: `out_color = hash3(uvec2(packetID, a_localVertexID));` —
  each packet renders a unique per-vertex hash pattern. If two packets
  share the same pattern, their packet-address derivation collided. If a
  single packet's pattern shifts frame-to-frame, the address is reading
  stale or racing data.

The whole class of `gl_VertexID` / `baseVertex` / packet-table-indexing
bugs is visible in minutes in this mode; without it, the same class of
bug typically takes days to isolate because it presents as "some shapes
look wrong, some look fine" mixed with other visual drift.

Required M1 gate: color-address validation passes before any appearance
type is declared shipped on the new path.

### In-game validation (both killswitch states, toggle via RAlt+0):
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
